#include "PluginEditor.h"
#include "PluginProcessor.h"

static void installSettingsIconTab(juce::TabbedComponent& tabs,
	int settingsTabIndex,
	std::unique_ptr<juce::Drawable> normal,
	std::unique_ptr<juce::Drawable> hover)
{
	struct CenteredIcon : public juce::Component, private juce::Timer
	{
		std::unique_ptr<juce::Drawable> normalImg, hoverImg;
		bool isOver = false;

		CenteredIcon(std::unique_ptr<juce::Drawable> n, std::unique_ptr<juce::Drawable> h)
			: normalImg(std::move(n)), hoverImg(std::move(h)) {
			startTimerHz(30);
		}
		void paint(juce::Graphics& g) override
		{
			const float scale = 0.4f;

			auto area = getParentComponent()
				? getParentComponent()->getLocalBounds().toFloat()
				: getLocalBounds().toFloat();

			auto iconArea = area.withSizeKeepingCentre(area.getWidth() * scale,
				area.getHeight() * scale);

			if (auto* d = (isOver && hoverImg ? hoverImg.get() : normalImg.get()))
				d->drawWithin(g, iconArea, juce::RectanglePlacement::centred, 1.0f);
		}
		void mouseEnter(const juce::MouseEvent&) override { isOver = true;  repaint(); }
		void mouseExit(const juce::MouseEvent&) override { isOver = false; repaint(); }

		void timerCallback() override
		{
			if (auto* p = getParentComponent())
				setBounds(p->getLocalBounds());
		}
	};

	auto& bar = tabs.getTabbedButtonBar();
	if (auto* btn = bar.getTabButton(settingsTabIndex))
	{
		btn->setButtonText(" ");

		auto* icon = new CenteredIcon(std::move(normal), std::move(hover));
		icon->setInterceptsMouseClicks(false, false);

		btn->setExtraComponent(icon, juce::TabBarButton::ExtraComponentPlacement::beforeText);
	}
}

LLMidiAudioProcessorEditor::LLMidiAudioProcessorEditor(LLMidiAudioProcessor& p)
	: AudioProcessorEditor(&p), audioProcessor(p)
{
	addAndMakeVisible(tabs);
	tabs.addTab("Offline", juce::Colours::darkgrey, new OfflinePage(), true);
	tabs.addTab("Online", juce::Colours::darkgrey, new OnlinePage(), true);

	tabs.addTab("Settings", juce::Colours::darkgrey, new SettingsPage(), true);
	settingsTabIndex = tabs.getNumTabs() - 1;
	settingsPage.reset(dynamic_cast<SettingsPage*>(tabs.getTabContentComponent(settingsTabIndex)));

	installSettingsIconTab(
		tabs,
		settingsTabIndex,
		IconLibrary::makeGearDrawable(juce::Colours::lightgrey),
		IconLibrary::makeGearDrawable(juce::Colours::darkgrey)
	);
	if (settingsPage)
	{
		settingsPage->setCacheSizeText(prettyBytes(getFolderSizeRecursive(cacheDirPath())));

		settingsPage->onDeleteCacheRequested = [this]
			{
				juce::AlertWindow::showOkCancelBox(
					juce::AlertWindow::WarningIcon,
					"Delete cache?",
					"Deleting the cache will remove all saved model prefix caches.\n"
					"This is safe, but the next generation with previously used models will be slower while the cache rebuilds.\n\n"
					"Do you want to proceed?",
					"Delete cache", "Cancel",
					this,
					juce::ModalCallbackFunction::create([this](int res)
						{
							if (res == 0) return; // Cancel
							auto dir = cacheDirPath();
							dir.deleteRecursively();
							dir.createDirectory();
							if (settingsPage) settingsPage->setCacheSizeText("0 B");
						})
				);
			};

		settingsPage->onThemeChanged = [this](int idx)
			{
				auto& lf = getLookAndFeel();
				switch (idx)
				{
				default:
				case 0:
				case 1: lf.setColour(juce::ResizableWindow::backgroundColourId, juce::Colours::darkgrey.darker(0.6f)); break;
				case 2: lf.setColour(juce::ResizableWindow::backgroundColourId, juce::Colours::black); break;
				}
				repaint();
			};
	}

	offlinePage.reset(dynamic_cast<OfflinePage*>(tabs.getTabContentComponent(0)));
	onlinePage.reset(dynamic_cast<OnlinePage*>(tabs.getTabContentComponent(1)));

	if (offlinePage)
	{
		offlinePage->onLoadModel = [this] { onClickLoadModel(); };
		offlinePage->onGenerate = [this] { onClickGenerate(); };
		offlinePage->onCopyLog = [this] { onClickCopyLog(); };
		offlinePage->onToggleLogs = [this] { onClickToggleLogs(); };
		offlinePage->onStop = [this] { onClickStop(); };
		offlinePage->onReroll = [this] { onClickReroll(); };
	}

	if (onlinePage)
	{
		onlinePage->onCopyPromptSucceeded = [this]
			{
				auto& mem = audioProcessor.getUiMemory();
				mem.onlineHasCopiedPrompt = true;
				saveUiMemoryToProcessor();
			};
		onlinePage->onResponseEdited = [this]
			{
				auto& mem = audioProcessor.getUiMemory();
				mem.onlineResponse = onlinePage->getResponseText();
				saveUiMemoryToProcessor();
			};
		onlinePage->onConvertToMidi = [this](const juce::String& text)
			{
				juce::String err;
				if (audioProcessor.importManualJson(text.toStdString(), err))
				{
					onlinePage->showStatus("Imported successfully!", juce::Colours::limegreen);
					onlinePage->setToMidiGlow(false);

					auto& mem = audioProcessor.getUiMemory();
					mem.onlineStatusText = "Imported successfully!";
					mem.onlineToMidiGlow = false;
					saveUiMemoryToProcessor();
				}
				else
				{
					onlinePage->showStatus("Error " + err, juce::Colours::orangered);
					auto& mem = audioProcessor.getUiMemory();
					mem.onlineStatusText = "Error " + err;
					saveUiMemoryToProcessor();
				}
			};
		onlinePage->onPromptEdited = [this]
			{
				auto& mem = audioProcessor.getUiMemory();
				mem.onlineHasCopiedPrompt = false;
				mem.onlineCopyGlow = true;
				mem.onlineToMidiGlow = false;
				mem.onlinePrompt = onlinePage->getPromptText();
				saveUiMemoryToProcessor();
			};

	}

	setSize(kWindowW, kBaseH);
	getLookAndFeel().setColour(juce::ResizableWindow::backgroundColourId,
		juce::Colours::darkgrey.darker(0.6f));
	logsVisible = false;
	if (offlinePage)
	{
		offlinePage->setLogsVisible(false);
		offlinePage->setProgressVisible(false);
		offlinePage->setStopVisible(false);

		const int restoredSeed = audioProcessor.getLastSeed();
		offlinePage->getSeedEditor().setText(restoredSeed < 0 ? "-1" : juce::String(restoredSeed),
			juce::dontSendNotification);
		offlinePage->getPromptEditor().setText("Piano melody on E minor", juce::dontSendNotification);
		audioProcessor.setLastPrompt("Piano melody on E minor");

		offlinePage->getSeedEditor().onTextChange = [this]
			{
				if (!offlinePage) return;
				const juce::String t = offlinePage->getSeedEditor().getText().trim();
				if (t.isEmpty() || t == "-1") { audioProcessor.setLastSeed(-1); return; }
				int s = t.getIntValue(); if (s <= 0) s = std::abs(s) + 1;
				audioProcessor.setLastSeed(s);
			};
	}

	if (offlinePage)
	{
		if (!audioProcessor.isModelReady())
		{
			offlinePage->setLoadGlow(true);
			offlinePage->setGenerateGlow(false);
		}
		else
		{
			offlinePage->setLoadGlow(false);
			offlinePage->setGenerateGlow(true);
		}
	}
	if (onlinePage)
	{
		loadUiMemoryFromProcessor();
	}

	syncUiFromProcessorOnce();

	lastSelectedTabIndex = tabs.getCurrentTabIndex();
	wasVisible = isVisible();

	startTimerHz(15);
}

LLMidiAudioProcessorEditor::~LLMidiAudioProcessorEditor() {
	tabs.getTabbedButtonBar().setLookAndFeel(nullptr);
}

void LLMidiAudioProcessorEditor::paint(juce::Graphics& g)
{
	g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void LLMidiAudioProcessorEditor::resized()
{
	tabs.setBounds(getLocalBounds());
}

void LLMidiAudioProcessorEditor::visibilityChanged()
{
	if (wasVisible && !isVisible())
	{
		saveUiMemoryToProcessor();
	}
	else if (!wasVisible && isVisible())
	{
		loadUiMemoryFromProcessor();
		syncUiFromProcessorOnce();
	}
	wasVisible = isVisible();
}

void LLMidiAudioProcessorEditor::timerCallback()
{
	lastSelectedTabIndex = tabs.getCurrentTabIndex();
	saveUiMemoryToProcessor();
	updateModelUi();
	updateGenProgress();
	updateLogView();
}

void LLMidiAudioProcessorEditor::syncUiFromProcessorOnce()
{
	if (!offlinePage) return;

	offlinePage->setLogsVisible(logsVisible);

	const juce::String bgLog = audioProcessor.getLlmLog();
	const bool done = bgLog.containsIgnoreCase("sequence ready");
	const bool midPrompt = bgLog.lastIndexOf("Prompt [") >= 0;
	const bool midGen = bgLog.lastIndexOf("Gen [") >= 0;

	if (done)
	{
		offlinePage->setProgressVisible(true);
		offlinePage->getProgressBar().setProgress(1.0);
		offlinePage->getProgressBar().setActive(false);
		offlinePage->setStageText("Ready to burn!", juce::Colours::white);
		offlinePage->setStopVisible(false);
		offlinePage->setGenerateGlow(false);
		generationActive = false;
		canceledThisRun = false;
	}
	else if (midPrompt || midGen)
	{
		offlinePage->setProgressVisible(true);
		offlinePage->getProgressBar().setActive(true);
		offlinePage->setStageText(midPrompt ? "Ingesting prompt..." : "Generating the pattern...", juce::Colours::white);
		offlinePage->setStopVisible(true);
		offlinePage->setGenerateGlow(false);
		generationActive = true;
		canceledThisRun = false;
	}
	else
	{
		offlinePage->setProgressVisible(false);
		offlinePage->setStopVisible(false);
		generationActive = false;

		if (audioProcessor.isModelReady())
		{
			offlinePage->setLoadGlow(false);
			offlinePage->setGenerateGlow(true);
		}
		else
		{
			offlinePage->setLoadGlow(true);
			offlinePage->setGenerateGlow(false);
		}
	}
}

void LLMidiAudioProcessorEditor::updateWindowSizeForLogs()
{
	setSize(kWindowW, logsVisible ? (kBaseH + kLogsExtraH) : kBaseH);
}

void LLMidiAudioProcessorEditor::updateModelUi()
{
	if (!offlinePage) return;

	const bool ready = audioProcessor.isModelReady();
	auto& dot = offlinePage->getModelDot();
	auto& nameLbl = offlinePage->getModelNameLabel();
	auto& loading = offlinePage->getModelLoadingLabel();

	if (ready)
	{
		dot.setColour(juce::Colours::limegreen);
		nameLbl.setText(juce::File(audioProcessor.getLoadedModelPath()).getFileName(), juce::dontSendNotification);
		modelLoadingFlag = false;
		loading.setVisible(false);

		offlinePage->setLoadGlow(false);
		offlinePage->setGenerateGlow(true);
	}
	else
	{
		dot.setColour(juce::Colours::red);
		if (modelLoadingFlag) loading.setVisible(true);

		offlinePage->setLoadGlow(true);
		offlinePage->setGenerateGlow(false);
	}
}

void LLMidiAudioProcessorEditor::updateGenProgress()
{
	if (!offlinePage) return;

	auto& bar = offlinePage->getProgressBar();
	auto& label = offlinePage->getStageLabel();

	const juce::String bgLog = audioProcessor.getLlmLog();

	if (canceledThisRun)
	{
		bar.setActive(false);
		return;
	}

	const bool hasParseErr = bgLog.containsIgnoreCase("parse error") ||
		bgLog.containsIgnoreCase("salvage also failed");
	const bool hasEmptyPhrase = bgLog.containsIgnoreCase("pattern gen: empty phrase after parse");
	const bool hasEmptyRaw = bgLog.containsIgnoreCase("pattern gen failed: empty raw output");
	const bool hasLlmErr = bgLog.containsIgnoreCase("llm error:");
	const bool decodeFail = bgLog.containsIgnoreCase("stop reason: decode_fail");
	const bool anyErr = hasParseErr || hasEmptyPhrase || hasEmptyRaw || hasLlmErr || decodeFail;

	if (anyErr)
	{
		generationActive = false;
		offlinePage->setProgressVisible(false);
		offlinePage->setStageText("Generation failed - check log.", juce::Colours::orangered);
		offlinePage->setStopVisible(false);
		offlinePage->setGenerateGlow(true);
		return;
	}

	const int  lastCache = bgLog.lastIndexOf("Building cache [");
	const int  lastPrompt = bgLog.lastIndexOf("Prompt [");
	const int  lastGen = bgLog.lastIndexOf("Gen [");
	const bool done = bgLog.containsIgnoreCase("sequence ready");
	const int  useIdx = juce::jmax(lastCache, juce::jmax(lastPrompt, lastGen));

	if (done)
	{
		bar.setProgress(1.0);
		bar.setActive(false);
		offlinePage->setStageText("Ready to burn!", juce::Colours::white);
		offlinePage->setStopVisible(false);
		offlinePage->setGenerateGlow(false);
		generationActive = false;
		return;
	}

	if (!generationActive)
	{
		if (useIdx >= 0)
		{
			generationActive = true;
			offlinePage->setProgressVisible(true);
			bar.setActive(true);
			offlinePage->setStopVisible(true);
			offlinePage->setGenerateGlow(false);
		}
		else
		{
			bar.setActive(false);
			return;
		}
	}

	// Update progress %
	if (useIdx >= 0)
	{
		const auto tail = bgLog.substring(useIdx);
		const int  open = tail.lastIndexOfChar('(');
		const int  close = tail.lastIndexOfChar(')');
		if (open >= 0 && close > open)
		{
			const auto percentStr = tail.substring(open + 1, close).removeCharacters("% ").trim();
			const int pct = percentStr.getIntValue();
			bar.setProgress(juce::jlimit(0, 100, pct) / 100.0);
		}

		if (useIdx == lastCache)
		{
			offlinePage->setStageText("Building cache...", juce::Colours::orange);
		}
		else if (useIdx == lastPrompt)
		{
			offlinePage->setStageText("Ingesting prompt...", juce::Colours::white);
		}
		else
		{
			offlinePage->setStageText("Generating the pattern...", juce::Colours::limegreen);
		}
		bar.setActive(true);
	}
}

void LLMidiAudioProcessorEditor::updateLogView()
{
	if (!offlinePage || !offlinePage->areLogsVisible())
		return;

	auto& le = offlinePage->getLogEditor();

	juce::String statusTop = audioProcessor.isModelReady() ? "Model ready.\n" : "Model not ready.\n";
	juce::String bgLog = audioProcessor.getLlmLog();

	juce::String combined;
	combined << statusTop
		<< "\n--- Background log ---\n"
		<< bgLog
		<< "\n";

	if (combined == lastRenderedLog)
		return;

	const bool userAtEnd = (le.getCaretPosition() >= le.getTotalNumChars() - 1);

	le.setText(combined, juce::dontSendNotification);
	if (userAtEnd)
	{
		le.moveCaretToEnd();
		le.scrollEditorToPositionCaret(0, le.getCaretRectangle().getY());
	}

	lastRenderedLog = combined;
}

void LLMidiAudioProcessorEditor::onClickLoadModel()
{
	modelChooser = std::make_unique<juce::FileChooser>("Select a GGUF model", juce::File(), "*.gguf");
	modelChooser->launchAsync(
		juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
		[this](const juce::FileChooser& chooser)
		{
			auto file = chooser.getResult();
			modelChooser.reset();

			if (!file.existsAsFile())
				return;

			audioProcessor.requestLoadModelFromFile(file);
			appendUiLogLine("[UI] Loading model: " + file.getFullPathName());

			modelLoadingFlag = true;
			if (offlinePage) offlinePage->getModelLoadingLabel().setVisible(true);

			if (offlinePage) offlinePage->setLoadGlow(false);
		});
}

void LLMidiAudioProcessorEditor::onClickCopyLog()
{
	if (!offlinePage) return;
	juce::SystemClipboard::copyTextToClipboard(offlinePage->getLogEditor().getText());
	appendUiLogLine("[UI] Log copied to clipboard.");
}

void LLMidiAudioProcessorEditor::onClickGenerate()
{
	if (!offlinePage) return;

	const juce::String promptText = offlinePage->getPromptEditor().getText().trim();
	if (promptText.isEmpty())
	{
		offlinePage->setPromptErrorGlow(true);
		offlinePage->setGenerateGlow(true);
		return;
	}

	offlinePage->setPromptErrorGlow(false);
	offlinePage->setGenerateGlow(false);

	const std::string naturalPrompt = promptText.toStdString();
	audioProcessor.setLastPrompt(promptText);

	const int bars = 8;
	const int stepsPerBar = 4;
	const int defaultVel = 96;
	const int channel = 0;

	const int seedToUse = readSeedOrRandom();
	audioProcessor.setLastSeed(seedToUse);
	audioProcessor.clearLlmLog();
	lastRenderedLog.clear();
	offlinePage->getLogEditor().clear();

	audioProcessor.requestLlmGeneratePattern(naturalPrompt, bars, stepsPerBar, defaultVel, channel, seedToUse);

	appendUiLogLine("[UI] Generate pattern requested with seed " + juce::String(seedToUse));
	offlinePage->getSeedEditor().setText(juce::String(seedToUse), juce::dontSendNotification);

	canceledThisRun = false;
	generationActive = true;

	offlinePage->setProgressVisible(true);
	offlinePage->getProgressBar().setProgress(0.0);
	offlinePage->getProgressBar().setActive(true);
	offlinePage->setStageText("Ingesting prompt...", juce::Colours::white);
	offlinePage->setStopVisible(true);
}

void LLMidiAudioProcessorEditor::onClickToggleLogs()
{
	logsVisible = !logsVisible;
	if (offlinePage) offlinePage->setLogsVisible(logsVisible);
	updateWindowSizeForLogs();
}

void LLMidiAudioProcessorEditor::onClickStop()
{
	audioProcessor.cancelLlmGeneration();
	canceledThisRun = true;
	generationActive = false;

	if (offlinePage)
	{
		offlinePage->getProgressBar().setActive(false);
		offlinePage->setProgressVisible(false);
		offlinePage->setStageText("Canceled", juce::Colours::white);
		offlinePage->setStopVisible(false);

		offlinePage->setGenerateGlow(true);
	}
}

void LLMidiAudioProcessorEditor::onClickReroll()
{
	if (!offlinePage) return;
	offlinePage->getSeedEditor().setText("-1", juce::dontSendNotification);
	audioProcessor.setLastSeed(-1);
	appendUiLogLine("[UI] Seed reset to -1 (reroll).");
}

void LLMidiAudioProcessorEditor::appendUiLogLine(const juce::String& line)
{
	if (!offlinePage) return;
	auto& le = offlinePage->getLogEditor();
	le.moveCaretToEnd();
	le.insertTextAtCaret(line + "\n");
	lastRenderedLog = le.getText();
}

int LLMidiAudioProcessorEditor::readSeedOrRandom() const
{
	if (!offlinePage) return 1;
	const juce::String text = offlinePage->getSeedEditor().getText().trim();
	if (text.isEmpty() || text == "-1")
	{
		int s = juce::Random::getSystemRandom().nextInt();
		if (s <= 0) s = std::abs(s) + 1;
		return s;
	}
	int s = text.getIntValue();
	if (s <= 0) s = std::abs(s) + 1;
	return s;
}
void LLMidiAudioProcessorEditor::saveUiMemoryToProcessor()
{
	auto& mem = audioProcessor.getUiMemory();
	mem.selectedTab = tabs.getCurrentTabIndex();

	if (onlinePage)
	{
		mem.onlinePrompt = onlinePage->getPromptText();
		mem.onlineResponse = onlinePage->getResponseText();
		mem.onlineStatusText = onlinePage->getStatusText();
		mem.onlineStatusColour = onlinePage->getStatusLabelColour();
		mem.onlineCopyGlow = onlinePage->isCopyGlowOn();
		mem.onlineToMidiGlow = onlinePage->isToMidiGlowOn();
		mem.onlinePromptErrorGlow = onlinePage->isPromptErrorGlowOn();
	}
}

void LLMidiAudioProcessorEditor::loadUiMemoryFromProcessor()
{
	const auto& mem = audioProcessor.getUiMemory();

	tabs.setCurrentTabIndex(juce::jlimit(0, tabs.getNumTabs() - 1, mem.selectedTab));

	if (onlinePage)
	{
		onlinePage->setPromptText(mem.onlinePrompt.isNotEmpty() ? mem.onlinePrompt : "Piano melody on E minor");
		onlinePage->setResponseText(mem.onlineResponse);
		if (mem.onlineStatusText.isNotEmpty())
			onlinePage->setStatusText(mem.onlineStatusText,
				mem.onlineStatusColour.isTransparent() ? juce::Colours::white
				: mem.onlineStatusColour);

		onlinePage->setPromptErrorGlow(mem.onlinePromptErrorGlow);
		onlinePage->setCopyGlow(mem.onlineCopyGlow);
		onlinePage->setToMidiGlow(mem.onlineToMidiGlow);
	}
}
juce::int64 LLMidiAudioProcessorEditor::getFolderSizeRecursive(const juce::File& dir)
{
	juce::int64 total = 0;
	if (!dir.isDirectory()) return 0;
	juce::Array<juce::File> entries;
	dir.findChildFiles(entries, juce::File::findFilesAndDirectories, false);
	for (auto f : entries)
		total += f.isDirectory() ? getFolderSizeRecursive(f) : f.getSize();
	return total;
}

juce::String LLMidiAudioProcessorEditor::prettyBytes(juce::int64 bytes)
{
	const double b = (double)bytes;
	if (b < 1024.0) return juce::String((int)b) + " B";
	const double kb = b / 1024.0;  if (kb < 1024.0) return juce::String(kb, 1) + " KB";
	const double mb = kb / 1024.0; if (mb < 1024.0) return juce::String(mb, 1) + " MB";
	const double gb = mb / 1024.0; return juce::String(gb, 2) + " GB";
}

juce::File LLMidiAudioProcessorEditor::cacheDirPath()
{
	auto d = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
		.getChildFile("LLMidi").getChildFile("cache");
	d.createDirectory();
	return d;
}