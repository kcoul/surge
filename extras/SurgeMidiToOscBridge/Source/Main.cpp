#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_osc/juce_osc.h>

#include <set>
#include <vector>

namespace
{
constexpr int defaultOscPort = 53280;
constexpr auto defaultTargetAddress = "127.0.0.1";

juce::String getNoteName (int noteNumber)
{
    return juce::MidiMessage::getMidiNoteName (noteNumber, true, true, 4);
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

        schemaLabel.setText ("Forwarded OSC: /mnote <note:float> <velocity:float>  (note-off = velocity 0)",
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
        schemaLabel.setBounds (area.removeFromTop (24));
        area.removeFromTop (6);
        statusLabel.setBounds (area.removeFromTop (24));
        area.removeFromTop (12);

        midiInputsLabel.setBounds (area.removeFromTop (24));
        midiInputsBox.setBounds (area.removeFromTop (110));
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
        if (! (message.isNoteOn() || message.isNoteOff()))
            return;

        const auto noteNumber = message.getNoteNumber();
        const auto deviceName = source != nullptr ? source->getName() : juce::String ("<unknown>");

        if (message.isNoteOn())
        {
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

    void sendSurgeNoteMessage (int noteNumber, int velocity)
    {
        sendOscMessage (juce::OSCMessage ("/mnote", (float) noteNumber, (float) velocity));
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
        settingsFile.saveIfNeeded();
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
    juce::Label schemaLabel;
    juce::Label statusLabel;
    juce::Label midiInputsLabel;
    juce::TextEditor midiInputsBox;
    juce::Label logLabel;
    juce::TextEditor logBox;
    juce::PropertiesFile& settingsFile;

    juce::CriticalSection oscLock;
    juce::CriticalSection activeNotesLock;
    juce::OSCSender oscSender;
    juce::String targetHost = defaultTargetAddress;
    int targetPort = defaultOscPort;
    std::vector<std::unique_ptr<juce::MidiInput>> midiInputs;
    std::set<int> activeNotes;

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
