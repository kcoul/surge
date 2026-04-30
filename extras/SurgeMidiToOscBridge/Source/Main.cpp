#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_osc/juce_osc.h>

#include <array>
#include <optional>
#include <set>
#include <vector>

namespace
{
constexpr int defaultOscPort = 53280;
constexpr int defaultEncoderStep = 8;
constexpr auto defaultTargetAddress = "127.0.0.1";

juce::String getNoteName (int noteNumber)
{
    return juce::MidiMessage::getMidiNoteName (noteNumber, true, true, 4);
}

enum class ControllerMode
{
    absolute,
    relative
};

enum class EncoderMode
{
    absolute = 1,
    twosComplement = 2,
    endpoint127Up = 3,
    endpoint0Up = 4
};

struct ControllerMapping
{
    int controllerNumber;
    const char* label;
    const char* oscAddress;
    ControllerMode mode;
    float currentValue;
};

constexpr std::array<ControllerMapping, 16> defaultControllerMappings {{
    { 12, "Fader 1 - Osc 1 Volume",       "/param/a/mixer/osc1/volume",    ControllerMode::absolute, 0.0f },
    { 13, "Fader 2 - Osc 2 Volume",       "/param/a/mixer/osc2/volume",    ControllerMode::absolute, 0.0f },
    { 14, "Fader 3 - Osc 3 Volume",       "/param/a/mixer/osc3/volume",    ControllerMode::absolute, 0.0f },
    { 15, "Fader 4 - Noise Volume",       "/param/a/mixer/noise/volume",   ControllerMode::absolute, 0.0f },
    { 16, "Fader 5 - Filter 1 Cutoff",    "/param/a/filter/1/cutoff",      ControllerMode::absolute, 0.0f },
    { 17, "Fader 6 - Filter 1 Resonance", "/param/a/filter/1/resonance",   ControllerMode::absolute, 0.0f },
    { 18, "Fader 7 - Filter 2 Cutoff",    "/param/a/filter/2/cutoff",      ControllerMode::absolute, 0.0f },
    { 19, "Fader 8 - Filter 2 Resonance", "/param/a/filter/2/resonance",   ControllerMode::absolute, 0.0f },
    { 22, "Encoder 1 - Filter Balance",   "/param/a/filter/balance",       ControllerMode::relative, 0.5f },
    { 23, "Encoder 2 - Filter Feedback",  "/param/a/filter/feedback",      ControllerMode::relative, 0.0f },
    { 24, "Encoder 3 - Waveshaper Drive", "/param/a/waveshaper/drive",     ControllerMode::relative, 0.0f },
    { 25, "Encoder 4 - Amp Attack",       "/param/a/aeg/attack",           ControllerMode::relative, 0.0f },
    { 26, "Encoder 5 - Amp Decay",        "/param/a/aeg/decay",            ControllerMode::relative, 0.5f },
    { 27, "Encoder 6 - Amp Sustain",      "/param/a/aeg/sustain",          ControllerMode::relative, 1.0f },
    { 28, "Encoder 7 - Amp Release",      "/param/a/aeg/release",          ControllerMode::relative, 0.25f },
    { 29, "Encoder 8 - Portamento",       "/param/a/portamento",           ControllerMode::relative, 0.0f },
}};

juce::String controllerModeName (ControllerMode mode)
{
    return mode == ControllerMode::absolute ? "absolute" : "relative";
}

juce::String encoderModeName (EncoderMode mode)
{
    switch (mode)
    {
    case EncoderMode::absolute:
        return "Absolute 0-127";
    case EncoderMode::twosComplement:
        return "Two's complement";
    case EncoderMode::endpoint127Up:
        return "Endpoint 0/127 (127 up)";
    case EncoderMode::endpoint0Up:
        return "Endpoint 0/127 (0 up)";
    }

    return "Unknown";
}

EncoderMode encoderModeFromId (int id)
{
    switch (id)
    {
    case static_cast<int> (EncoderMode::absolute):
        return EncoderMode::absolute;
    case static_cast<int> (EncoderMode::twosComplement):
        return EncoderMode::twosComplement;
    case static_cast<int> (EncoderMode::endpoint0Up):
        return EncoderMode::endpoint0Up;
    case static_cast<int> (EncoderMode::endpoint127Up):
    default:
        return EncoderMode::endpoint127Up;
    }
}

std::optional<int> relativeControllerDelta (int value, EncoderMode encoderMode)
{
    switch (encoderMode)
    {
    case EncoderMode::absolute:
        return std::nullopt;

    case EncoderMode::twosComplement:
        // Common two's-complement relative CC encoding: 1..63 increment, 65..127 decrement.
        if (value >= 1 && value <= 63)
            return value;

        if (value >= 65 && value <= 127)
            return value - 128;

        return std::nullopt;

    case EncoderMode::endpoint127Up:
        if (value == 127)
            return 1;

        if (value == 0)
            return -1;

        return std::nullopt;

    case EncoderMode::endpoint0Up:
        if (value == 0)
            return 1;

        if (value == 127)
            return -1;

        return std::nullopt;
    }

    return std::nullopt;
}
}

class MainComponent final : public juce::Component,
                            private juce::MidiInputCallback
{
public:
    explicit MainComponent (juce::PropertiesFile& settingsFileToUse)
        : settingsFile (settingsFileToUse)
    {
        setSize (920, 680);

        titleLabel.setText ("Surge MIDI To OSC Bridge", juce::dontSendNotification);
        titleLabel.setFont (juce::FontOptions (28.0f));
        titleLabel.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (titleLabel);

        targetIpLabel.setText ("Target IP", juce::dontSendNotification);
        addAndMakeVisible (targetIpLabel);

        targetIpEditor.setText (settingsFile.getValue ("targetIp", defaultTargetAddress),
                                juce::dontSendNotification);
        addAndMakeVisible (targetIpEditor);

        targetPortLabel.setText ("OSC Port", juce::dontSendNotification);
        addAndMakeVisible (targetPortLabel);

        targetPortEditor.setText (settingsFile.getValue ("targetPort",
                                                         juce::String (defaultOscPort)),
                                  juce::dontSendNotification);
        addAndMakeVisible (targetPortEditor);

        connectButton.setButtonText ("Connect");
        connectButton.onClick = [this] { connectOscSender(); };
        addAndMakeVisible (connectButton);

        refreshMidiButton.setButtonText ("Refresh MIDI");
        refreshMidiButton.onClick = [this] { refreshMidiInputs(); };
        addAndMakeVisible (refreshMidiButton);

        allNotesOffButton.setButtonText ("All Notes Off");
        allNotesOffButton.onClick = [this] { sendAllNotesOff(); };
        addAndMakeVisible (allNotesOffButton);

        encoderModeLabel.setText ("Encoder Mode", juce::dontSendNotification);
        addAndMakeVisible (encoderModeLabel);

        encoderModeBox.addItem (encoderModeName (EncoderMode::endpoint127Up),
                                static_cast<int> (EncoderMode::endpoint127Up));
        encoderModeBox.addItem (encoderModeName (EncoderMode::endpoint0Up),
                                static_cast<int> (EncoderMode::endpoint0Up));
        encoderModeBox.addItem (encoderModeName (EncoderMode::twosComplement),
                                static_cast<int> (EncoderMode::twosComplement));
        encoderModeBox.addItem (encoderModeName (EncoderMode::absolute),
                                static_cast<int> (EncoderMode::absolute));

        encoderMode = encoderModeFromId (settingsFile.getIntValue ("encoderMode",
                                                                   static_cast<int> (encoderMode)));
        encoderModeBox.setSelectedId (static_cast<int> (encoderMode), juce::dontSendNotification);
        encoderModeBox.onChange = [this] { updateEncoderModeFromComboBox(); };
        addAndMakeVisible (encoderModeBox);

        encoderStepLabel.setText ("Encoder Step", juce::dontSendNotification);
        addAndMakeVisible (encoderStepLabel);

        for (auto step : { 1, 2, 4, 8, 16, 32 })
            encoderStepBox.addItem (juce::String (step), step);

        encoderStep = juce::jlimit (1, 32,
                                    settingsFile.getIntValue ("encoderStep", defaultEncoderStep));
        encoderStepBox.setSelectedId (encoderStep, juce::dontSendNotification);
        encoderStepBox.onChange = [this] { updateEncoderStepFromComboBox(); };
        addAndMakeVisible (encoderStepBox);

        schemaLabel.setText ("Forwarded OSC: /mnote plus CC12-19/22-29 mapped to /param/a/... float values",
                             juce::dontSendNotification);
        addAndMakeVisible (schemaLabel);

        statusLabel.setText ("Disconnected", juce::dontSendNotification);
        statusLabel.setColour (juce::Label::textColourId, juce::Colours::darkred);
        addAndMakeVisible (statusLabel);

        midiInputsLabel.setText ("Open MIDI inputs", juce::dontSendNotification);
        addAndMakeVisible (midiInputsLabel);

        midiInputsBox.setMultiLine (true);
        midiInputsBox.setReadOnly (true);
        midiInputsBox.setColour (juce::TextEditor::backgroundColourId, juce::Colours::black);
        midiInputsBox.setColour (juce::TextEditor::textColourId, juce::Colours::white);
        midiInputsBox.setColour (juce::TextEditor::outlineColourId, juce::Colour::fromRGB (88, 102, 114));
        addAndMakeVisible (midiInputsBox);

        controllerMappingsLabel.setText ("Controller mappings", juce::dontSendNotification);
        addAndMakeVisible (controllerMappingsLabel);

        controllerMappingsBox.setMultiLine (true);
        controllerMappingsBox.setReadOnly (true);
        controllerMappingsBox.setColour (juce::TextEditor::backgroundColourId, juce::Colours::black);
        controllerMappingsBox.setColour (juce::TextEditor::textColourId, juce::Colours::white);
        controllerMappingsBox.setColour (juce::TextEditor::outlineColourId,
                                         juce::Colour::fromRGB (88, 102, 114));
        controllerMappingsBox.setText (getControllerMappingsDescription(), juce::dontSendNotification);
        addAndMakeVisible (controllerMappingsBox);

        logLabel.setText ("Event log", juce::dontSendNotification);
        addAndMakeVisible (logLabel);

        logBox.setMultiLine (true);
        logBox.setReadOnly (true);
        logBox.setScrollbarsShown (true);
        logBox.setCaretVisible (false);
        logBox.setColour (juce::TextEditor::backgroundColourId, juce::Colours::black);
        logBox.setColour (juce::TextEditor::textColourId, juce::Colours::white);
        logBox.setColour (juce::TextEditor::outlineColourId, juce::Colour::fromRGB (88, 102, 114));
        addAndMakeVisible (logBox);

        connectOscSender();
        refreshMidiInputs();
    }

    ~MainComponent() override
    {
        sendAllNotesOff();
        saveSettings();
        shutdownMidiInputs();

        const juce::ScopedLock lock (oscLock);
        oscSender.disconnect();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour::fromRGB (234, 228, 219));

        auto panel = getLocalBounds().toFloat().reduced (16.0f);
        g.setColour (juce::Colour::fromRGB (31, 48, 61));
        g.fillRoundedRectangle (panel, 22.0f);

        g.setColour (juce::Colour::fromRGBA (255, 255, 255, 24));
        g.fillRoundedRectangle (panel.removeFromTop (86.0f), 22.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (28);

        titleLabel.setBounds (area.removeFromTop (42));
        area.removeFromTop (12);

        auto topRow = area.removeFromTop (30);
        targetIpLabel.setBounds (topRow.removeFromLeft (80));
        targetIpEditor.setBounds (topRow.removeFromLeft (230));
        topRow.removeFromLeft (16);
        targetPortLabel.setBounds (topRow.removeFromLeft (70));
        targetPortEditor.setBounds (topRow.removeFromLeft (80));
        topRow.removeFromLeft (16);
        connectButton.setBounds (topRow.removeFromLeft (100));
        topRow.removeFromLeft (10);
        refreshMidiButton.setBounds (topRow.removeFromLeft (120));
        topRow.removeFromLeft (10);
        allNotesOffButton.setBounds (topRow.removeFromLeft (120));

        area.removeFromTop (10);
        auto encoderRow = area.removeFromTop (30);
        encoderModeLabel.setBounds (encoderRow.removeFromLeft (100));
        encoderModeBox.setBounds (encoderRow.removeFromLeft (260));
        encoderRow.removeFromLeft (18);
        encoderStepLabel.setBounds (encoderRow.removeFromLeft (100));
        encoderStepBox.setBounds (encoderRow.removeFromLeft (100));

        area.removeFromTop (8);
        schemaLabel.setBounds (area.removeFromTop (24));
        area.removeFromTop (6);
        statusLabel.setBounds (area.removeFromTop (24));
        area.removeFromTop (12);

        midiInputsLabel.setBounds (area.removeFromTop (24));
        midiInputsBox.setBounds (area.removeFromTop (88));
        area.removeFromTop (12);
        controllerMappingsLabel.setBounds (area.removeFromTop (24));
        controllerMappingsBox.setBounds (area.removeFromTop (154));
        area.removeFromTop (14);
        logLabel.setBounds (area.removeFromTop (24));
        area.removeFromTop (6);
        logBox.setBounds (area);
    }

private:
    void connectOscSender()
    {
        const auto ipAddress = targetIpEditor.getText().trim();
        const auto port = targetPortEditor.getText().getIntValue();

        if (ipAddress.isEmpty() || port <= 0)
        {
            setStatus ("Invalid OSC target", juce::Colours::darkred);
            appendLog ("OSC connect failed: invalid target IP or port");
            return;
        }

        const juce::ScopedLock lock (oscLock);
        oscSender.disconnect();

        if (oscSender.connect (ipAddress, port))
        {
            targetHost = ipAddress;
            targetPort = port;
            saveSettings();
            setStatus ("Connected to " + targetHost + ":" + juce::String (targetPort),
                       juce::Colours::darkgreen);
            appendLog ("OSC connected to " + targetHost + ":" + juce::String (targetPort));
        }
        else
        {
            setStatus ("OSC connection failed", juce::Colours::darkred);
            appendLog ("OSC connection failed for " + ipAddress + ":" + juce::String (port));
        }
    }

    void refreshMidiInputs()
    {
        shutdownMidiInputs();

        const auto devices = juce::MidiInput::getAvailableDevices();
        juce::StringArray openedDevices;

        for (const auto& device : devices)
        {
            if (auto input = juce::MidiInput::openDevice (device.identifier, this))
            {
                input->start();
                openedDevices.add (device.name);
                midiInputs.push_back (std::move (input));
            }
            else
            {
                appendLog ("Failed to open MIDI input: " + device.name);
            }
        }

        if (openedDevices.isEmpty())
        {
            midiInputsBox.setText ("No MIDI input devices opened.", juce::dontSendNotification);
            appendLog ("No MIDI inputs available");
        }
        else
        {
            midiInputsBox.setText (openedDevices.joinIntoString ("\n"), juce::dontSendNotification);
            appendLog ("Opened MIDI inputs:\n" + openedDevices.joinIntoString ("\n"));
        }
    }

    void shutdownMidiInputs()
    {
        for (auto& input : midiInputs)
            input->stop();

        midiInputs.clear();
    }

    void handleIncomingMidiMessage (juce::MidiInput* source, const juce::MidiMessage& message) override
    {
        if (! (message.isNoteOn() || message.isNoteOff() || message.isController()))
            return;

        const auto deviceName = source != nullptr ? source->getName() : juce::String ("<unknown>");

        if (message.isController())
        {
            handleControllerMessage (deviceName, message.getControllerNumber(),
                                     message.getControllerValue());
        }
        else if (message.isNoteOn())
        {
            const auto noteNumber = message.getNoteNumber();
            const auto velocity = message.getVelocity();
            sendSurgeNoteMessage (noteNumber, velocity);
            {
                const juce::ScopedLock lock (activeNotesLock);
                activeNotes.insert (noteNumber);
            }
            appendLogAsync ("MIDI noteOn from " + deviceName
                            + ": " + getNoteName (noteNumber)
                            + " (" + juce::String (noteNumber) + ")"
                            + " velocity=" + juce::String (velocity));
        }
        else
        {
            const auto noteNumber = message.getNoteNumber();
            sendSurgeNoteMessage (noteNumber, 0);
            {
                const juce::ScopedLock lock (activeNotesLock);
                activeNotes.erase (noteNumber);
            }
            appendLogAsync ("MIDI noteOff from " + deviceName
                            + ": " + getNoteName (noteNumber)
                            + " (" + juce::String (noteNumber) + ")");
        }
    }

    void handleControllerMessage (const juce::String& deviceName, int controllerNumber, int controllerValue)
    {
        ControllerMapping mappingToSend {};
        float oscValue = 0.0f;

        {
            const juce::ScopedLock lock (controllerMappingsLock);

            auto* mapping = findControllerMapping (controllerNumber);
            if (mapping == nullptr)
                return;

            oscValue = controllerValue / 127.0f;

            if (mapping->mode == ControllerMode::relative)
            {
                if (encoderMode == EncoderMode::absolute)
                {
                    oscValue = controllerValue / 127.0f;
                }
                else
                {
                    auto delta = relativeControllerDelta (controllerValue, encoderMode);
                    if (! delta.has_value())
                        return;

                    oscValue = juce::jlimit (0.0f, 1.0f,
                                             mapping->currentValue
                                                 + ((*delta * encoderStep) / 127.0f));
                }
            }

            mapping->currentValue = oscValue;
            mappingToSend = *mapping;
        }

        sendSurgeParameterMessage (mappingToSend, oscValue);

        appendLogAsync ("MIDI CC" + juce::String (controllerNumber)
                        + " from " + deviceName
                        + " -> " + juce::String (mappingToSend.oscAddress)
                        + " raw=" + juce::String (controllerValue)
                        + " step=" + juce::String (encoderStep)
                        + " value=" + juce::String (oscValue, 3));
    }

    ControllerMapping* findControllerMapping (int controllerNumber)
    {
        for (auto& mapping : controllerMappings)
            if (mapping.controllerNumber == controllerNumber)
                return &mapping;

        return nullptr;
    }

    void sendSurgeNoteMessage (int noteNumber, int velocity)
    {
        sendOscMessage (juce::OSCMessage ("/mnote", (float) noteNumber, (float) velocity));
    }

    void sendSurgeParameterMessage (const ControllerMapping& mapping, float normalizedValue)
    {
        sendOscMessage (juce::OSCMessage (mapping.oscAddress, normalizedValue));
    }

    void sendAllNotesOff()
    {
        std::vector<int> notes;
        {
            const juce::ScopedLock lock (activeNotesLock);
            notes.assign (activeNotes.begin(), activeNotes.end());
            activeNotes.clear();
        }

        for (auto note : notes)
            sendSurgeNoteMessage (note, 0);

        if (! notes.empty())
            appendLogAsync ("Sent all-notes-off for " + juce::String ((int) notes.size()) + " active notes");
    }

    void sendOscMessage (const juce::OSCMessage& message)
    {
        const juce::ScopedLock lock (oscLock);

        if (! oscSender.send (message))
        {
            appendLogAsync ("Failed to send OSC " + message.getAddressPattern().toString()
                            + " to " + targetHost + ":" + juce::String (targetPort));
        }
    }

    void setStatus (const juce::String& text, juce::Colour colour)
    {
        statusLabel.setText (text, juce::dontSendNotification);
        statusLabel.setColour (juce::Label::textColourId, colour);
    }

    void saveSettings()
    {
        settingsFile.setValue ("targetIp", targetIpEditor.getText().trim());
        settingsFile.setValue ("targetPort", targetPortEditor.getText().trim());
        settingsFile.setValue ("encoderMode", static_cast<int> (encoderMode));
        settingsFile.setValue ("encoderStep", encoderStep);
        settingsFile.saveIfNeeded();
    }

    void updateEncoderModeFromComboBox()
    {
        {
            const juce::ScopedLock lock (controllerMappingsLock);
            encoderMode = encoderModeFromId (encoderModeBox.getSelectedId());
        }

        saveSettings();
        controllerMappingsBox.setText (getControllerMappingsDescription(), juce::dontSendNotification);
        appendLog ("Encoder mode set to " + encoderModeName (encoderMode));
    }

    void updateEncoderStepFromComboBox()
    {
        {
            const juce::ScopedLock lock (controllerMappingsLock);
            encoderStep = juce::jlimit (1, 32, encoderStepBox.getSelectedId());
        }

        saveSettings();
        controllerMappingsBox.setText (getControllerMappingsDescription(), juce::dontSendNotification);
        appendLog ("Encoder step set to " + juce::String (encoderStep));
    }

    juce::String getControllerMappingsDescription() const
    {
        juce::StringArray lines;
        lines.add ("Encoder mode: " + encoderModeName (encoderMode));
        lines.add ("Encoder step: " + juce::String (encoderStep) + " MIDI counts per tick");
        lines.add ("");

        for (const auto& mapping : controllerMappings)
        {
            lines.add ("CC" + juce::String (mapping.controllerNumber)
                       + " (" + controllerModeName (mapping.mode) + ")  "
                       + mapping.label + "  ->  " + mapping.oscAddress);
        }

        return lines.joinIntoString ("\n");
    }

    void appendLog (const juce::String& line)
    {
        const auto timestamp = juce::Time::getCurrentTime().formatted ("%H:%M:%S");
        logBox.moveCaretToEnd();
        logBox.insertTextAtCaret ("[" + timestamp + "] " + line + "\n");
    }

    void appendLogAsync (juce::String line)
    {
        juce::MessageManager::callAsync ([this, line = std::move (line)]
        {
            appendLog (line);
        });
    }

    juce::Label titleLabel;
    juce::Label targetIpLabel;
    juce::TextEditor targetIpEditor;
    juce::Label targetPortLabel;
    juce::TextEditor targetPortEditor;
    juce::TextButton connectButton;
    juce::TextButton refreshMidiButton;
    juce::TextButton allNotesOffButton;
    juce::Label encoderModeLabel;
    juce::ComboBox encoderModeBox;
    juce::Label encoderStepLabel;
    juce::ComboBox encoderStepBox;
    juce::Label schemaLabel;
    juce::Label statusLabel;
    juce::Label midiInputsLabel;
    juce::TextEditor midiInputsBox;
    juce::Label controllerMappingsLabel;
    juce::TextEditor controllerMappingsBox;
    juce::Label logLabel;
    juce::TextEditor logBox;
    juce::PropertiesFile& settingsFile;

    juce::CriticalSection oscLock;
    juce::CriticalSection activeNotesLock;
    juce::CriticalSection controllerMappingsLock;
    juce::OSCSender oscSender;
    juce::String targetHost = defaultTargetAddress;
    int targetPort = defaultOscPort;
    EncoderMode encoderMode = EncoderMode::absolute;
    int encoderStep = defaultEncoderStep;
    std::vector<std::unique_ptr<juce::MidiInput>> midiInputs;
    std::set<int> activeNotes;
    std::array<ControllerMapping, 16> controllerMappings = defaultControllerMappings;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

class SurgeMidiToOscBridgeApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override       { return "Surge MIDI To OSC Bridge"; }
    const juce::String getApplicationVersion() override    { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override             { return true; }

    void initialise (const juce::String&) override
    {
        juce::PropertiesFile::Options options;
        options.applicationName = "SurgeMidiToOscBridge";
        options.filenameSuffix = "settings";
        options.osxLibrarySubFolder = "Application Support";
        options.folderName = "Surge Synth Team";

        appProperties.setStorageParameters (options);
        mainWindow = std::make_unique<MainWindow> (getApplicationName(), *appProperties.getUserSettings());
    }

    void shutdown() override
    {
        mainWindow.reset();
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    void anotherInstanceStarted (const juce::String&) override {}

    class MainWindow final : public juce::DocumentWindow
    {
    public:
        MainWindow (juce::String name, juce::PropertiesFile& settingsFile)
            : DocumentWindow (std::move (name),
                              juce::Colour::fromRGB (234, 228, 219),
                              juce::DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setResizable (true, false);
            setResizeLimits (780, 560, 1800, 1400);
            setContentOwned (new MainComponent (settingsFile), true);
            setSize (920, 680);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

private:
    juce::ApplicationProperties appProperties;
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (SurgeMidiToOscBridgeApplication)
