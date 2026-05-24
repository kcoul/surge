#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_osc/juce_osc.h>
#ifndef BRIDGE_HAS_HAILO
#define BRIDGE_HAS_HAILO 0
#endif
#if BRIDGE_HAS_HAILO
// Windows COM headers (included transitively via HailoRT's platform.h →
// winsock2.h → basetyps.h) define `interface` as `struct`.  This conflicts
// with HailoRT's use of `interface` as a parameter name in hailort_defaults.hpp.
//
// Workaround: pre-define the hailort_defaults.hpp include guard so that
// hailort.hpp skips it on its own inclusion pass.  After hailort.hpp is done
// (and Windows headers are now processed), undefine both the fake guard and
// the COM macro, then include hailort_defaults.hpp ourselves in a clean state.
//
// This avoids modifying the installed SDK headers.
#ifdef _WIN32
#  define _HAILO_HAILORT_DEFAULTS_HPP_   // suppress inclusion by hailort.hpp
#endif
#include <hailo/genai/speech2text/speech2text.hpp>
#include <hailo/hailort.hpp>
#ifdef _WIN32
#  undef _HAILO_HAILORT_DEFAULTS_HPP_    // allow our explicit inclusion below
#  undef interface                        // remove COM's `#define interface struct`
#  include <hailo/hailort_defaults.hpp>  // now parses cleanly
#endif
#endif
#include <whisper.h>
#include "MidiLearnMap.h"

#ifndef BRIDGE_HAS_EMBEDDED_VAD
#define BRIDGE_HAS_EMBEDDED_VAD 0
#endif

#if BRIDGE_HAS_EMBEDDED_VAD
#include "embedded_vad_model.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cctype>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace
{
#if BRIDGE_HAS_EMBEDDED_VAD
struct EmbeddedBufferState
{
    const unsigned char* data;
    size_t               size;
    size_t               pos { 0 };
};

static size_t embeddedBufferRead (void* ctx, void* out, size_t n)
{
    auto* s = static_cast<EmbeddedBufferState*> (ctx);
    const auto avail  = s->size - s->pos;
    const auto toRead = std::min (n, avail);
    std::memcpy (out, s->data + s->pos, toRead);
    s->pos += toRead;
    return toRead;
}

static bool embeddedBufferEof (void* ctx)
{
    auto* s = static_cast<EmbeddedBufferState*> (ctx);
    return s->pos >= s->size;
}

static void embeddedBufferClose (void*) {}
#endif

#if BRIDGE_HAS_HAILO
// Locate a Whisper HEF without relying on hailo_apps ResourcesManager.
// Search order: models/hailo10h/ next to the binary, ~/bridge/models/hailo10h/,
// then ~/bridge/ directly. Throws with a clear message if not found.
static std::string findHailoWhisperHef (const std::string& hefFilename)
{
    const auto binary = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    const auto home   = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    const auto name   = juce::String (hefFilename);

    const juce::File candidates[] = {
        binary.getParentDirectory().getChildFile ("models/hailo10h").getChildFile (name),
        home.getChildFile ("bridge/models/hailo10h").getChildFile (name),
        home.getChildFile ("bridge").getChildFile (name),
    };

    for (const auto& f : candidates)
        if (f.existsAsFile())
            return f.getFullPathName().toStdString();

    throw std::runtime_error (
        "Hailo Whisper HEF not found: " + hefFilename +
        ". Place it in ~/bridge/ or next to the binary in models/hailo10h/");
}
#endif

constexpr int defaultOscPort       = 53280;  // SurgeXT OSC input port
constexpr int defaultSurgeOscOutPort = 53281; // SurgeXT OSC output port (Bridge listens here)
constexpr int defaultEncoderStep = 8;
constexpr int whisperSampleRate = 16000;
constexpr double vadFrameSeconds = 0.04;
constexpr double vadPreRollSeconds = 0.25;
constexpr double vadMinSpeechSeconds = 0.30;
constexpr double vadEndSilenceSeconds = 0.28;
constexpr double vadMaxUtteranceSeconds = 2.5;
constexpr float vadSpeechThreshold = 0.50f;
constexpr auto defaultTargetAddress = "127.0.0.1";
constexpr auto tinyModelPath  = "libs/whisper.cpp/models/ggml-tiny.bin";
constexpr auto baseModelPath  = "libs/whisper.cpp/models/ggml-base.bin";
constexpr auto smallModelPath = "libs/whisper.cpp/models/ggml-small.bin";
constexpr auto vadModelPath = "libs/whisper.cpp/models/ggml-silero-v6.2.0.bin";
#if BRIDGE_HAS_HAILO
constexpr auto hailoVDeviceGroupId = "SHARED";
#endif

#ifndef SURGE_SOURCE_DIR
#define SURGE_SOURCE_DIR "."
#endif

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

struct VoiceCommandAction
{
    const char* label;
    const char* oscAddress;
};

enum class VoiceTranscriptionBackend
{
    whisperCpu = 1,
    whisperGpu = 2,
    hailoNpu = 3
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

[[maybe_unused]] juce::String controllerModeName (ControllerMode mode)
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

juce::String normalizeCommandText (juce::String text)
{
    text = text.toLowerCase();

    juce::String normalized;
    bool lastWasSpace = true;

    for (auto c : text)
    {
        if (std::isalnum (static_cast<unsigned char> (c)))
        {
            normalized += juce::String::charToString (c);
            lastWasSpace = false;
        }
        else if (! lastWasSpace)
        {
            normalized += " ";
            lastWasSpace = true;
        }
    }

    return normalized.trim();
}

juce::String voiceBackendName (VoiceTranscriptionBackend backend)
{
    switch (backend)
    {
    case VoiceTranscriptionBackend::whisperCpu:
        return "whisper.cpp CPU";
    case VoiceTranscriptionBackend::whisperGpu:
        return "whisper.cpp GPU";
    case VoiceTranscriptionBackend::hailoNpu:
        return "Hailo NPU";
    }

    return "Unknown";
}

const char* voiceBackendTranscriptId (VoiceTranscriptionBackend backend)
{
    switch (backend)
    {
    case VoiceTranscriptionBackend::whisperCpu:
        return "whisper-cpu";
    case VoiceTranscriptionBackend::whisperGpu:
        return "whisper-gpu";
    case VoiceTranscriptionBackend::hailoNpu:
        return "hailo-npu";
    }

    return "unknown";
}

VoiceTranscriptionBackend defaultVoiceBackend()
{
#if BRIDGE_HAS_HAILO
    return VoiceTranscriptionBackend::hailoNpu;
#else
    return VoiceTranscriptionBackend::whisperCpu;
#endif
}

VoiceTranscriptionBackend voiceBackendFromId (int id)
{
    switch (id)
    {
    case static_cast<int> (VoiceTranscriptionBackend::whisperCpu):
        return VoiceTranscriptionBackend::whisperCpu;
    case static_cast<int> (VoiceTranscriptionBackend::whisperGpu):
        return VoiceTranscriptionBackend::whisperGpu;
    case static_cast<int> (VoiceTranscriptionBackend::hailoNpu):
#if BRIDGE_HAS_HAILO
        return VoiceTranscriptionBackend::hailoNpu;
#else
        return defaultVoiceBackend();
#endif
    default:
        return defaultVoiceBackend();
    }
}

bool isWhisperCppBackend (VoiceTranscriptionBackend backend)
{
    return backend == VoiceTranscriptionBackend::whisperCpu
        || backend == VoiceTranscriptionBackend::whisperGpu;
}

std::optional<VoiceCommandAction> voiceCommandActionForText (const juce::String& normalized)
{
    if (normalized.contains ("previous bank"))
        return VoiceCommandAction { "Previous Bank", "/patch/decr_category" };

    if (normalized.contains ("next bank"))
        return VoiceCommandAction { "Next Bank", "/patch/incr_category" };

    if (normalized.contains ("previous patch"))
        return VoiceCommandAction { "Previous Patch", "/patch/decr" };

    if (normalized.contains ("next patch"))
        return VoiceCommandAction { "Next Patch", "/patch/incr" };

    if (normalized.contains ("surprise me"))
        return VoiceCommandAction { "Surprise Me!", "/patch/random" };

    return std::nullopt;
}

void whisperBridgeLogCallback (enum ggml_log_level level, const char* text, void*)
{
    if (level < GGML_LOG_LEVEL_WARN || text == nullptr)
        return;

    std::fputs (text, stderr);
    std::fflush (stderr);
}

#if BRIDGE_HAS_HAILO
std::shared_ptr<hailort::VDevice> createSharedHailoVDevice()
{
    hailo_vdevice_params_t params {};
    const auto status = hailo_init_vdevice_params (&params);
    if (status != HAILO_SUCCESS)
        throw hailort::hailort_error (status, "Failed to initialize Hailo VDevice params");

    params.group_id = hailoVDeviceGroupId;

    auto vdevice = hailort::VDevice::create_shared (params);
    if (! vdevice)
        throw hailort::hailort_error (vdevice.status(), "Failed to create shared Hailo VDevice");

    return vdevice.release();
}
#endif
}

class MainComponent final : public juce::Component,
                            private juce::MidiInputCallback,
                            private juce::AudioIODeviceCallback,
                            private juce::OSCReceiver::Listener<juce::OSCReceiver::MessageLoopCallback>,
                            private juce::Timer
{
public:
    explicit MainComponent (juce::PropertiesFile& settingsFileToUse)
        : settingsFile (settingsFileToUse)
    {
        whisper_log_set (whisperBridgeLogCallback, nullptr);

        setSize (920, 820);

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

        patchNavigationLabel.setText ("Patch navigation", juce::dontSendNotification);
        addAndMakeVisible (patchNavigationLabel);

        previousBankButton.setButtonText ("Previous Bank");
        previousBankButton.onClick = [this] {
            triggerPatchNavigationAction ("Previous Bank", "/patch/decr_category");
        };
        addAndMakeVisible (previousBankButton);

        nextBankButton.setButtonText ("Next Bank");
        nextBankButton.onClick = [this] {
            triggerPatchNavigationAction ("Next Bank", "/patch/incr_category");
        };
        addAndMakeVisible (nextBankButton);

        previousProgramButton.setButtonText ("Previous Patch");
        previousProgramButton.onClick = [this] {
            triggerPatchNavigationAction ("Previous Patch", "/patch/decr");
        };
        addAndMakeVisible (previousProgramButton);

        nextProgramButton.setButtonText ("Next Patch");
        nextProgramButton.onClick = [this] {
            triggerPatchNavigationAction ("Next Patch", "/patch/incr");
        };
        addAndMakeVisible (nextProgramButton);

        randomPatchButton.setButtonText ("Surprise Me!");
        randomPatchButton.onClick = [this] {
            triggerPatchNavigationAction ("Surprise Me!", "/patch/random");
        };
        addAndMakeVisible (randomPatchButton);

        randomParamsButton.setButtonText ("Randomize");
        randomParamsButton.onClick = [this] { randomizeAllParams(); };
        addAndMakeVisible (randomParamsButton);

        voiceLabel.setText ("Voice", juce::dontSendNotification);
        addAndMakeVisible (voiceLabel);

        voiceBackendBox.addItem (voiceBackendName (VoiceTranscriptionBackend::whisperCpu),
                                 static_cast<int> (VoiceTranscriptionBackend::whisperCpu));
        voiceBackendBox.addItem (voiceBackendName (VoiceTranscriptionBackend::whisperGpu),
                                 static_cast<int> (VoiceTranscriptionBackend::whisperGpu));
#if BRIDGE_HAS_HAILO
        voiceBackendBox.addItem (voiceBackendName (VoiceTranscriptionBackend::hailoNpu),
                                 static_cast<int> (VoiceTranscriptionBackend::hailoNpu));
#endif
        const auto savedBackend = voiceBackendFromId (
            settingsFile.getIntValue ("voiceBackend",
                                      static_cast<int> (defaultVoiceBackend())));
        voiceBackendBox.setSelectedId (
            static_cast<int> (savedBackend),
            juce::dontSendNotification);
        voiceBackendBox.onChange = [this] { saveSettings(); };
        addAndMakeVisible (voiceBackendBox);

        voiceModelBox.addItem ("tiny",  1);
        voiceModelBox.addItem ("base",  2);
        voiceModelBox.addItem ("small", 3);

        {
            const bool tinyAvailable  = resolveModelFile (tinyModelPath).existsAsFile();
            const bool baseAvailable  = resolveModelFile (baseModelPath).existsAsFile();
            const bool smallAvailable = resolveModelFile (smallModelPath).existsAsFile();
            voiceModelBox.setItemEnabled (1, tinyAvailable);
            voiceModelBox.setItemEnabled (2, baseAvailable);
            voiceModelBox.setItemEnabled (3, smallAvailable);

            int savedId = settingsFile.getIntValue ("voiceModel", 1);
            if ((savedId == 1 && ! tinyAvailable) || (savedId == 2 && ! baseAvailable) || (savedId == 3 && ! smallAvailable))
                savedId = tinyAvailable ? 1 : (baseAvailable ? 2 : (smallAvailable ? 3 : 0));
            voiceModelBox.setSelectedId (savedId, juce::dontSendNotification);
        }

        voiceModelBox.onChange = [this] { saveSettings(); };
        addAndMakeVisible (voiceModelBox);

        voiceToggleButton.setButtonText ("Start Voice");
        voiceToggleButton.onClick = [this] { toggleVoiceRecognition(); };
        addAndMakeVisible (voiceToggleButton);

        voiceStatusLabel.setText ("Voice stopped", juce::dontSendNotification);
        addAndMakeVisible (voiceStatusLabel);

        voiceRecognizedBox.setMultiLine (false);
        voiceRecognizedBox.setReadOnly (true);
        voiceRecognizedBox.setColour (juce::TextEditor::backgroundColourId, juce::Colours::black);
        voiceRecognizedBox.setColour (juce::TextEditor::textColourId, juce::Colours::white);
        voiceRecognizedBox.setColour (juce::TextEditor::outlineColourId,
                                      juce::Colour::fromRGB (88, 102, 114));
        voiceRecognizedBox.setText ("", juce::dontSendNotification);
        addAndMakeVisible (voiceRecognizedBox);

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

        logBox.setMultiLine (true);
        logBox.setReadOnly (true);
        logBox.setScrollbarsShown (true);
        logBox.setCaretVisible (false);
        logBox.setColour (juce::TextEditor::backgroundColourId, juce::Colours::black);
        logBox.setColour (juce::TextEditor::textColourId, juce::Colours::white);
        logBox.setColour (juce::TextEditor::outlineColourId, juce::Colour::fromRGB (88, 102, 114));

        transcriptBox.setMultiLine (true);
        transcriptBox.setReadOnly (true);
        transcriptBox.setScrollbarsShown (true);
        transcriptBox.setCaretVisible (false);
        transcriptBox.setColour (juce::TextEditor::backgroundColourId, juce::Colours::black);
        transcriptBox.setColour (juce::TextEditor::textColourId, juce::Colours::white);
        transcriptBox.setColour (juce::TextEditor::outlineColourId,
                                 juce::Colour::fromRGB (88, 102, 114));

        debugLogBox.setMultiLine (true);
        debugLogBox.setReadOnly (true);
        debugLogBox.setScrollbarsShown (true);
        debugLogBox.setCaretVisible (false);
        debugLogBox.setColour (juce::TextEditor::backgroundColourId, juce::Colours::black);
        debugLogBox.setColour (juce::TextEditor::textColourId, juce::Colour::fromRGB (180, 220, 140));
        debugLogBox.setColour (juce::TextEditor::outlineColourId, juce::Colour::fromRGB (88, 102, 114));

        logTabs.addTab ("Debug Log",      juce::Colour::fromRGB (31, 48, 61), &debugLogBox,    false);
        logTabs.addTab ("MIDI Event Log", juce::Colour::fromRGB (31, 48, 61), &logBox,         false);
        logTabs.addTab ("MIDI Learn",     juce::Colour::fromRGB (31, 48, 61), &midiLearnPanel, false);
        logTabs.addTab ("Transcript",     juce::Colour::fromRGB (31, 48, 61), &transcriptBox,  false);
        addAndMakeVisible (logTabs);

        // Wire up MIDI Learn panel callbacks
        midiLearnPanel.onQueryRequested = [this]
        {
            {
                const juce::ScopedLock lock (midiLearnLock);
                discoveredParamAddresses.clear();
            }
            midiLearnPanel.refresh();
            sendOscMessage (juce::OSCMessage ("/q/all_params"));
            appendLog ("Sent /q/all_params — waiting for parameter list...");
        };

        midiLearnPanel.onLearnRequested = [this] (int row)
        {
            learningRow.store (row);
            midiLearnPanel.setLearningRow (row);
        };

        midiLearnPanel.onClearRequested = [this] (int row)
        {
            {
                const juce::ScopedLock lock (midiLearnLock);
                if (row >= 0 && row < discoveredParamAddresses.size())
                    learnMap.clear (discoveredParamAddresses.getReference (row));
            }
            saveMidiLearnMap();
            midiLearnPanel.refresh();
        };

        midiLearnPanel.onModeToggleRequested = [this] (int row)
        {
            {
                const juce::ScopedLock lock (midiLearnLock);
                if (row >= 0 && row < discoveredParamAddresses.size())
                {
                    auto& b = learnMap.bindings[discoveredParamAddresses.getReference (row)];
                    b.mode = (b.mode == LearnMode::absolute) ? LearnMode::relative
                                                             : LearnMode::absolute;
                }
            }
            saveMidiLearnMap();
            midiLearnPanel.refresh();
        };

        loadMidiLearnMap();
        connectOscSender();
        refreshMidiInputs();
    }

    ~MainComponent() override
    {
        stopVoiceRecognition();
        sendAllNotesOff();
        saveSettings();
        shutdownMidiInputs();
        oscReceiver.removeListener (this);
        oscReceiver.disconnect();

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
        auto patchNavRow = area.removeFromTop (30);
        patchNavigationLabel.setBounds (patchNavRow.removeFromLeft (120));
        previousBankButton.setBounds (patchNavRow.removeFromLeft (124));
        patchNavRow.removeFromLeft (8);
        nextBankButton.setBounds (patchNavRow.removeFromLeft (96));
        patchNavRow.removeFromLeft (12);
        previousProgramButton.setBounds (patchNavRow.removeFromLeft (130));
        patchNavRow.removeFromLeft (8);
        nextProgramButton.setBounds (patchNavRow.removeFromLeft (104));
        patchNavRow.removeFromLeft (12);
        randomPatchButton.setBounds (patchNavRow.removeFromLeft (116));
        patchNavRow.removeFromLeft (12);
        randomParamsButton.setBounds (patchNavRow.removeFromLeft (100));

        area.removeFromTop (8);
        auto voiceRow = area.removeFromTop (30);
        voiceLabel.setBounds (voiceRow.removeFromLeft (64));
        voiceBackendBox.setBounds (voiceRow.removeFromLeft (150));
        voiceRow.removeFromLeft (8);
        voiceModelBox.setBounds (voiceRow.removeFromLeft (88));
        voiceRow.removeFromLeft (8);
        voiceToggleButton.setBounds (voiceRow.removeFromLeft (116));
        voiceRow.removeFromLeft (12);
        voiceStatusLabel.setBounds (voiceRow.removeFromLeft (150));
        voiceRow.removeFromLeft (8);
        voiceRecognizedBox.setBounds (voiceRow);

        area.removeFromTop (8);
        statusLabel.setBounds (area.removeFromTop (24));
        area.removeFromTop (12);

        midiInputsLabel.setBounds (area.removeFromTop (24));
        midiInputsBox.setBounds (area.removeFromTop (88));
        area.removeFromTop (12);
        logTabs.setBounds (area);
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

            // Start OSC receiver so we can hear SurgeXT's /q/all_params response.
            // SurgeXT sends replies to its configured output port (default 53281).
            // TODO: expose this port in the UI when multi-machine setups are needed.
            oscReceiver.removeListener (this);
            oscReceiver.disconnect();
            if (oscReceiver.connect (defaultSurgeOscOutPort))
            {
                oscReceiver.addListener (this);
                appendLog ("OSC receiver listening on port "
                           + juce::String (defaultSurgeOscOutPort));
                sendOscMessage (juce::OSCMessage ("/q/all_params"));
                appendLog ("Sent /q/all_params - discovering parameters...");
            }
            else
            {
                appendLog ("OSC receiver failed on port "
                           + juce::String (defaultSurgeOscOutPort)
                           + " (use Query button in MIDI Learn tab to retry)");
            }
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

    void toggleVoiceRecognition()
    {
        if (voiceEnabled.load())
            stopVoiceRecognition();
        else
            startVoiceRecognition();
    }

    void startVoiceRecognition()
    {
        if (voiceEnabled.load())
            return;

        const auto backend = voiceBackendFromId (voiceBackendBox.getSelectedId());
        const auto modelPath = selectedVoiceModelPath();
        const auto voiceVadModelPath = selectedVoiceVadModelPath();

        if (isWhisperCppBackend (backend) && ! juce::File (modelPath).existsAsFile())
        {
            voiceStatusLabel.setText ("Model missing", juce::dontSendNotification);
            appendLog ("Voice start failed: model file not found: " + modelPath);
            return;
        }

#if !BRIDGE_HAS_EMBEDDED_VAD
        if (! juce::File (voiceVadModelPath).existsAsFile())
        {
            voiceStatusLabel.setText ("VAD model missing", juce::dontSendNotification);
            appendLog ("Voice start failed: VAD model file not found: " + voiceVadModelPath);
            return;
        }
#endif

        const auto result = audioDeviceManager.initialise (1, 0, nullptr, true);
        if (result.isNotEmpty())
        {
            voiceStatusLabel.setText ("Mic unavailable", juce::dontSendNotification);
            appendLog ("Voice start failed: " + result);
            return;
        }

        {
            std::lock_guard<std::mutex> lock (voiceMutex);
            micInputBuffer.clear();
            micSampleRate = whisperSampleRate;
        }

        transcriptBox.clear();
        appendTranscriptLine ("[voice settings] vad_frame="
                              + juce::String (vadFrameSeconds, 2)
                              + "s pre_roll=" + juce::String (vadPreRollSeconds, 2)
                              + "s end_silence=" + juce::String (vadEndSilenceSeconds, 2)
                              + "s threshold=" + juce::String (vadSpeechThreshold, 2)
                              + " backend=" + voiceBackendTranscriptId (backend));

        voiceWorkerShouldStop.store (false);
        voiceEnabled.store (true);
        audioDeviceManager.addAudioCallback (this);
        voiceWorker = std::thread ([this,
                                    backend,
                                    modelPathStr = modelPath.toStdString(),
                                    voiceVadModelPathStr = voiceVadModelPath.toStdString(),
                                    hailoHefName = selectedHailoHefName()] {
            voiceWorkerLoop (backend, modelPathStr, voiceVadModelPathStr, hailoHefName);
        });

        voiceToggleButton.setButtonText ("Stop Voice");
        voiceStatusLabel.setText ("Voice listening", juce::dontSendNotification);
        appendLog ("Voice recognition started with " + voiceBackendName (backend)
                   + " and VAD model " + voiceVadModelPath);
    }

    void stopVoiceRecognition()
    {
        if (! voiceEnabled.exchange (false))
            return;

        audioDeviceManager.removeAudioCallback (this);
        audioDeviceManager.closeAudioDevice();

        {
            std::lock_guard<std::mutex> lock (voiceMutex);
            voiceWorkerShouldStop.store (true);
        }
        voiceCv.notify_all();

        if (voiceWorker.joinable())
            voiceWorker.join();

        voiceToggleButton.setButtonText ("Start Voice");
        voiceStatusLabel.setText ("Voice stopped", juce::dontSendNotification);
        appendLog ("Voice recognition stopped");
    }

    // Resolve a model file path. Checks a 'models/' directory next to the binary
    // first (portable/deployed layout), then falls back to the source-relative
    // path baked in at build time (development builds running from the build tree).
    juce::File resolveModelFile (const char* repoRelativePath) const
    {
        const auto modelName = juce::File (SURGE_SOURCE_DIR).getChildFile (repoRelativePath).getFileName();
        const auto nextToBinary = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                                      .getParentDirectory()
                                      .getChildFile ("models")
                                      .getChildFile (modelName);

        if (nextToBinary.existsAsFile())
            return nextToBinary;

        return juce::File (SURGE_SOURCE_DIR).getChildFile (repoRelativePath);
    }

    juce::String selectedVoiceModelPath() const
    {
        const auto relativePath = voiceModelBox.getSelectedId() == 3 ? smallModelPath
                                : voiceModelBox.getSelectedId() == 2 ? baseModelPath
                                                                      : tinyModelPath;
        return resolveModelFile (relativePath).getFullPathName();
    }

    juce::String selectedVoiceVadModelPath() const
    {
        return resolveModelFile (vadModelPath).getFullPathName();
    }

    std::string selectedHailoHefName() const
    {
        const auto id = voiceModelBox.getSelectedId();
        if (id == 3) return "Whisper-Small.hef";
        if (id == 2) return "Whisper-Base.hef";
        return "Whisper-Tiny.hef";
    }

    void audioDeviceAboutToStart (juce::AudioIODevice* device) override
    {
        std::lock_guard<std::mutex> lock (voiceMutex);
        micSampleRate = device != nullptr ? device->getCurrentSampleRate() : whisperSampleRate;
        micInputBuffer.clear();
    }

    void audioDeviceStopped() override
    {
        std::lock_guard<std::mutex> lock (voiceMutex);
        micInputBuffer.clear();
    }

    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (outputChannelData[ch] != nullptr)
                juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

        if (! voiceEnabled.load() || numInputChannels <= 0 || numSamples <= 0)
            return;

        std::vector<float> mono;
        mono.resize (static_cast<size_t> (numSamples));

        for (int i = 0; i < numSamples; ++i)
        {
            float sum = 0.0f;
            int channelsUsed = 0;

            for (int ch = 0; ch < numInputChannels; ++ch)
            {
                if (inputChannelData[ch] != nullptr)
                {
                    sum += inputChannelData[ch][i];
                    ++channelsUsed;
                }
            }

            mono[static_cast<size_t> (i)] = channelsUsed > 0 ? sum / static_cast<float> (channelsUsed)
                                                             : 0.0f;
        }

        {
            std::lock_guard<std::mutex> lock (voiceMutex);
            micInputBuffer.insert (micInputBuffer.end(), mono.begin(), mono.end());
        }
        voiceCv.notify_one();
    }

    void voiceWorkerLoop (VoiceTranscriptionBackend backend,
                          const std::string& modelPath,
                          [[maybe_unused]] const std::string& voiceVadModelPath,
                          [[maybe_unused]] const std::string& hailoHefName)
    {
        auto vadContextParams = whisper_vad_default_context_params();
        vadContextParams.n_threads = juce::jlimit (1, 4,
                                                   static_cast<int> (std::thread::hardware_concurrency()));
        vadContextParams.use_gpu = false;

#if BRIDGE_HAS_EMBEDDED_VAD
        EmbeddedBufferState vadBufState { ggml_silero_vad_model, ggml_silero_vad_model_size };
        whisper_model_loader vadLoader { &vadBufState,
                                         embeddedBufferRead,
                                         embeddedBufferEof,
                                         embeddedBufferClose };
        std::unique_ptr<whisper_vad_context, decltype (&whisper_vad_free)> vadCtx (
            whisper_vad_init_with_params (&vadLoader, vadContextParams),
            whisper_vad_free);
#else
        std::unique_ptr<whisper_vad_context, decltype (&whisper_vad_free)> vadCtx (
            whisper_vad_init_from_file_with_params (voiceVadModelPath.c_str(), vadContextParams),
            whisper_vad_free);
#endif

        if (vadCtx == nullptr)
        {
            juce::MessageManager::callAsync ([this] {
                appendLog ("Voice recognition failed: unable to load VAD model");
                stopVoiceRecognition();
                voiceStatusLabel.setText ("VAD load failed", juce::dontSendNotification);
            });
            return;
        }

        std::unique_ptr<whisper_context, decltype (&whisper_free)> whisperCtx (nullptr,
                                                                               whisper_free);
#if BRIDGE_HAS_HAILO
        std::shared_ptr<hailort::VDevice> hailoVDevice;
        std::unique_ptr<hailort::genai::Speech2Text> speech2Text;
#endif

        if (isWhisperCppBackend (backend))
        {
            auto cparams = whisper_context_default_params();
            cparams.use_gpu = backend == VoiceTranscriptionBackend::whisperGpu;

            whisperCtx.reset (whisper_init_from_file_with_params (modelPath.c_str(), cparams));
            if (whisperCtx == nullptr)
            {
                juce::MessageManager::callAsync ([this] {
                    appendLog ("Voice recognition failed: unable to load Whisper model");
                    stopVoiceRecognition();
                    voiceStatusLabel.setText ("Model load failed", juce::dontSendNotification);
                });
                return;
            }

            appendTranscriptLine ("[whisper.cpp init] model=\"" + juce::String (modelPath)
                                  + "\" gpu=" + juce::String (cparams.use_gpu ? 1 : 0));
        }
        else
        {
#if BRIDGE_HAS_HAILO
            try
            {
                const auto hefPath = findHailoWhisperHef (hailoHefName);

                hailoVDevice = createSharedHailoVDevice();
                auto speech2TextParams = hailort::genai::Speech2TextParams (hefPath);
                auto speech2TextExpected = hailort::genai::Speech2Text::create (hailoVDevice,
                                                                                speech2TextParams);

                if (! speech2TextExpected)
                    throw hailort::hailort_error (speech2TextExpected.status(),
                                                  "Failed to create Hailo Speech2Text");

                speech2Text = std::make_unique<hailort::genai::Speech2Text> (
                    speech2TextExpected.release());

                appendTranscriptLine ("[hailo init] hef=\"" + juce::String (hefPath) + "\"");
            }
            catch (const std::exception& exception)
            {
                juce::MessageManager::callAsync ([this, message = juce::String (exception.what())] {
                    appendLog ("Voice recognition failed: " + message);
                    stopVoiceRecognition();
                    voiceStatusLabel.setText ("Hailo load failed", juce::dontSendNotification);
                });
                return;
            }
#else
            juce::MessageManager::callAsync ([this] {
                appendLog ("Voice recognition failed: Hailo NPU backend is not available in this build");
                stopVoiceRecognition();
                voiceStatusLabel.setText ("Backend unavailable", juce::dontSendNotification);
            });
            return;
#endif
        }

        {
            std::lock_guard<std::mutex> lock (voiceMutex);
            micInputBuffer.clear();
        }
        whisper_vad_reset_state (vadCtx.get());
        appendTranscriptLine ("[voice ready] mic buffer cleared after backend init");

        std::vector<float> preRoll;
        std::vector<float> utterance;
        bool speechActive = false;
        size_t silenceSamples = 0;
        size_t speechFrameSamples = 0;  // actual spoken samples, excluding preroll

        const auto preRollSamples = static_cast<size_t> (vadPreRollSeconds * whisperSampleRate);
        const auto minSpeechSamples = static_cast<size_t> (vadMinSpeechSeconds * whisperSampleRate);
        const auto endSilenceSamples = static_cast<size_t> (vadEndSilenceSeconds * whisperSampleRate);
        const auto maxUtteranceSamples = static_cast<size_t> (vadMaxUtteranceSeconds * whisperSampleRate);

        while (! voiceWorkerShouldStop.load())
        {
            std::vector<float> chunk;
            double sampleRate = whisperSampleRate;

            {
                std::unique_lock<std::mutex> lock (voiceMutex);
                voiceCv.wait (lock, [this] {
                    const auto needed = static_cast<size_t> (micSampleRate * vadFrameSeconds);
                    return voiceWorkerShouldStop.load() || micInputBuffer.size() >= needed;
                });

                if (voiceWorkerShouldStop.load())
                    break;

                sampleRate = micSampleRate;
                const auto needed = static_cast<size_t> (sampleRate * vadFrameSeconds);

                if (micInputBuffer.size() < needed)
                    continue;

                const auto maxBufferedSamples = needed * 20;
                if (! speechActive && micInputBuffer.size() > maxBufferedSamples)
                {
                    appendTranscriptLine (
                        "[drop backlog] dropped_samples="
                        + juce::String ((int) (micInputBuffer.size() - maxBufferedSamples)));
                    micInputBuffer.erase (
                        micInputBuffer.begin(),
                        micInputBuffer.end() - static_cast<std::ptrdiff_t> (maxBufferedSamples));
                }

                chunk.assign (micInputBuffer.begin(),
                              micInputBuffer.begin() + static_cast<std::ptrdiff_t> (needed));
                micInputBuffer.erase (micInputBuffer.begin(),
                                      micInputBuffer.begin() + static_cast<std::ptrdiff_t> (needed));
            }

            auto pcm16k = resampleToWhisperRate (chunk, sampleRate);
            if (pcm16k.empty())
                continue;

            if (! whisper_vad_detect_speech_no_reset (vadCtx.get(), pcm16k.data(),
                                                       static_cast<int> (pcm16k.size())))
                continue;

            const auto probabilityCount = whisper_vad_n_probs (vadCtx.get());
            const auto probabilities = whisper_vad_probs (vadCtx.get());
            auto maxSpeechProbability = 0.0f;

            for (int i = 0; probabilities != nullptr && i < probabilityCount; ++i)
                maxSpeechProbability = std::max (maxSpeechProbability, probabilities[i]);

            const auto frameHasSpeech = maxSpeechProbability >= vadSpeechThreshold;

            if (! speechActive)
            {
                preRoll.insert (preRoll.end(), pcm16k.begin(), pcm16k.end());
                if (preRoll.size() > preRollSamples)
                    preRoll.erase (preRoll.begin(),
                                   preRoll.end() - static_cast<std::ptrdiff_t> (preRollSamples));

                if (! frameHasSpeech)
                {
                    handleVoiceSilence();
                    continue;
                }

                speechActive = true;
                silenceSamples = 0;
                speechFrameSamples = pcm16k.size();
                utterance = preRoll;
                appendTranscriptLine ("[vad start] prob="
                                      + juce::String (maxSpeechProbability, 2)
                                      + " preroll_samples=" + juce::String ((int) preRoll.size()));
                juce::MessageManager::callAsync ([this] {
                    voiceStatusLabel.setText ("Voice active", juce::dontSendNotification);
                });
            }
            else
            {
                utterance.insert (utterance.end(), pcm16k.begin(), pcm16k.end());
                if (frameHasSpeech)
                    speechFrameSamples += pcm16k.size();
            }

            if (frameHasSpeech)
                silenceSamples = 0;
            else
                silenceSamples += pcm16k.size();

            const auto hasEnoughSpeech = speechFrameSamples >= minSpeechSamples;
            const auto reachedEndSilence = silenceSamples >= endSilenceSamples;
            const auto reachedMaxDuration = utterance.size() >= maxUtteranceSamples;

            if (! reachedMaxDuration && (! hasEnoughSpeech || ! reachedEndSilence))
                continue;

            appendTranscriptLine (juce::String (reachedMaxDuration ? "[vad max] " : "[vad end] ")
                                  + "samples=" + juce::String ((int) utterance.size()));

            appendTranscriptLine ("[whisper submit] samples=" + juce::String ((int) utterance.size())
                                  + " audio_ms="
                                  + juce::String ((1000.0 * static_cast<double> (utterance.size()))
                                                  / static_cast<double> (whisperSampleRate), 1)
                                  + " speech_ms="
                                  + juce::String ((1000.0 * static_cast<double> (speechFrameSamples))
                                                  / static_cast<double> (whisperSampleRate), 1)
                                  + " backend=" + voiceBackendTranscriptId (backend));
            const auto whisperStart = std::chrono::steady_clock::now();
            juce::String text;

            try
            {
                if (isWhisperCppBackend (backend))
                    text = transcribeVoiceChunk (whisperCtx.get(), utterance);
#if BRIDGE_HAS_HAILO
                else
                    text = transcribeVoiceChunk (*speech2Text, utterance);
#else
                else
                    throw std::runtime_error ("Hailo NPU backend is not available in this build");
#endif
            }
            catch (const std::exception& exception)
            {
                appendTranscriptLine ("[whisper error] backend="
                                      + juce::String (voiceBackendTranscriptId (backend))
                                      + " message=\"" + juce::String (exception.what()) + "\"");

                if (! isWhisperCppBackend (backend))
                {
                    juce::MessageManager::callAsync ([this] {
                        appendLog ("Voice recognition stopped: Hailo NPU transcription failed");
                        stopVoiceRecognition();
                        voiceStatusLabel.setText ("Hailo error", juce::dontSendNotification);
                    });
                    return;
                }

                continue;
            }

            const auto whisperElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds> (
                std::chrono::steady_clock::now() - whisperStart);
            appendTranscriptLine ("[whisper result] elapsed_ms="
                                  + juce::String (static_cast<int> (whisperElapsedMs.count()))
                                  + " text=\"" + text + "\"");

            handleRecognizedVoiceText (text);

            speechActive = false;
            silenceSamples = 0;
            speechFrameSamples = 0;
            utterance.clear();
            preRoll.clear();
            whisper_vad_reset_state (vadCtx.get());
        }
    }

    std::vector<float> resampleToWhisperRate (const std::vector<float>& input, double inputRate) const
    {
        if (input.empty())
            return {};

        if (std::abs (inputRate - whisperSampleRate) < 1.0)
            return input;

        const auto outputSize = static_cast<size_t> (
            std::max (1.0, std::floor ((static_cast<double> (input.size()) * whisperSampleRate)
                                       / inputRate)));

        std::vector<float> output (outputSize);
        const auto ratio = inputRate / static_cast<double> (whisperSampleRate);

        for (size_t i = 0; i < output.size(); ++i)
        {
            const auto sourcePos = static_cast<double> (i) * ratio;
            const auto index = static_cast<size_t> (sourcePos);
            const auto frac = static_cast<float> (sourcePos - static_cast<double> (index));
            const auto a = input[std::min (index, input.size() - 1)];
            const auto b = input[std::min (index + 1, input.size() - 1)];
            output[i] = a + ((b - a) * frac);
        }

        return output;
    }

    void appendTranscriptLine (juce::String line)
    {
        juce::MessageManager::callAsync ([this, transcriptLine = std::move (line)] {
            const auto timestamp = juce::Time::getCurrentTime().formatted ("%H:%M:%S");
            transcriptBox.moveCaretToEnd();
            transcriptBox.insertTextAtCaret ("[" + timestamp + "] " + transcriptLine + "\n");
        });
    }

#if BRIDGE_HAS_HAILO
    juce::String transcribeVoiceChunk (hailort::genai::Speech2Text& speech2Text,
                                       const std::vector<float>& samples)
    {
        if (samples.empty())
            return {};

        auto generatorParams = speech2Text.create_generator_params()
                                   .expect ("Failed to create Hailo Speech2Text generator params");

        auto status = generatorParams.set_task (hailort::genai::Speech2TextTask::TRANSCRIBE);
        if (status != HAILO_SUCCESS)
            throw hailort::hailort_error (status, "Failed to set Hailo Speech2Text task");

        status = generatorParams.set_language ("en");
        if (status != HAILO_SUCCESS)
            throw hailort::hailort_error (status, "Failed to set Hailo Speech2Text language");

        const auto text = speech2Text.generate_all_text (
            hailort::MemoryView (const_cast<float*> (samples.data()),
                                 samples.size() * sizeof (float)),
            generatorParams,
            std::chrono::milliseconds (15000))
                              .expect ("Failed to generate Hailo transcription");

        return juce::String (text).trim();
    }
#endif

    juce::String transcribeVoiceChunk (whisper_context* ctx, const std::vector<float>& samples)
    {
        if (ctx == nullptr || samples.empty())
            return {};

        auto params = whisper_full_default_params (WHISPER_SAMPLING_GREEDY);
        params.n_threads = juce::jlimit (1, 4, static_cast<int> (std::thread::hardware_concurrency()));
        params.no_context = true;
        params.no_timestamps = true;
        params.single_segment = true;
        params.max_tokens = 16;
        params.audio_ctx = 512;
        params.print_special = false;
        params.print_progress = false;
        params.print_realtime = false;
        params.print_timestamps = false;
        params.language = "en";
        params.suppress_blank = true;
        params.suppress_nst = true;
        params.temperature = 0.0f;

        if (whisper_full (ctx, params, samples.data(), static_cast<int> (samples.size())) != 0)
            return {};

        juce::String text;
        const auto segments = whisper_full_n_segments (ctx);
        for (int i = 0; i < segments; ++i)
            text += whisper_full_get_segment_text (ctx, i);

        return text.trim();
    }

    void handleVoiceSilence()
    {
        bool shouldUpdateStatus = false;

        {
            std::lock_guard<std::mutex> lock (voiceCommandMutex);
            if (lastVoiceCommandNormalized.isNotEmpty())
            {
                lastVoiceCommandNormalized = {};
                shouldUpdateStatus = true;
            }
        }

        if (shouldUpdateStatus)
        {
            juce::MessageManager::callAsync ([this] {
                voiceStatusLabel.setText ("Voice listening", juce::dontSendNotification);
            });
        }
    }

    void handleRecognizedVoiceText (juce::String text)
    {
        if (text.isEmpty())
            return;

        const auto normalized = normalizeCommandText (text);
        const auto action = voiceCommandActionForText (normalized);

        if (! action.has_value())
        {
            juce::MessageManager::callAsync ([this, text] {
                voiceRecognizedBox.setText (text, juce::dontSendNotification);
                voiceStatusLabel.setText ("No command match", juce::dontSendNotification);
                appendLog ("Voice heard: \"" + text + "\"");
            });
            return;
        }

        {
            std::lock_guard<std::mutex> lock (voiceCommandMutex);
            if (normalized == lastVoiceCommandNormalized)
            {
                juce::MessageManager::callAsync ([this, label = juce::String (action->label)] {
                    voiceStatusLabel.setText ("Already handled: " + label,
                                              juce::dontSendNotification);
                });
                return;
            }

            lastVoiceCommandNormalized = normalized;
        }

        sendOscMessage (juce::OSCMessage (action->oscAddress));

        juce::MessageManager::callAsync ([this,
                                          text,
                                          label = juce::String (action->label),
                                          oscAddress = juce::String (action->oscAddress)] {
            voiceRecognizedBox.setText (text, juce::dontSendNotification);
            voiceStatusLabel.setText ("Matched: " + label, juce::dontSendNotification);
            appendLog ("Voice matched: \"" + text + "\" -> OSC: " + oscAddress);
        });
    }

    void handleIncomingMidiMessage (juce::MidiInput* source, const juce::MidiMessage& message) override
    {
        if (! (message.isNoteOn() || message.isNoteOff() || message.isController()
               || message.isProgramChange()))
            return;

        const auto deviceName = source != nullptr ? source->getName() : juce::String ("<unknown>");

        if (message.isController())
        {
            if (handleBankSelectMessage (deviceName, message.getControllerNumber(),
                                         message.getControllerValue()))
                return;

            handleControllerMessage (message.getControllerNumber(),
                                     message.getControllerValue());
        }
        else if (message.isProgramChange())
        {
            handleProgramChangeMessage (deviceName, message.getProgramChangeNumber());
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
            appendMidiLogAsync ("MIDI noteOn from " + deviceName
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
            appendMidiLogAsync ("MIDI noteOff from " + deviceName
                               + ": " + getNoteName (noteNumber)
                               + " (" + juce::String (noteNumber) + ")");
        }
    }

    void handleControllerMessage (int controllerNumber, int controllerValue)
    {
        // ── MIDI Learn: capture mode ─────────────────────────────────────────
        // learningRow is atomic; store -1 immediately so a second CC doesn't
        // double-fire before the async UI update below reaches the message thread.
        const int captureRow = learningRow.exchange (-1);
        if (captureRow >= 0)
        {
            juce::String address;
            {
                const juce::ScopedLock lock (midiLearnLock);
                if (captureRow < discoveredParamAddresses.size())
                    address = discoveredParamAddresses.getReference (captureRow);
            }
            if (address.isNotEmpty())
            {
                const int cc = controllerNumber;
                {
                    const juce::ScopedLock lock (midiLearnLock);
                    learnMap.assign (address, 0, cc);
                }
                juce::MessageManager::callAsync ([this] {
                    midiLearnPanel.setLearningRow (-1);
                    midiLearnPanel.refresh();
                    saveMidiLearnMap();
                });
                appendLogAsync ("MIDI Learn bound: CC" + juce::String (cc)
                                + " -> " + address);
                return;
            }
            // Row was out of range — cancel learn mode cleanly
            juce::MessageManager::callAsync ([this] { midiLearnPanel.setLearningRow (-1); });
        }

        // ── MIDI Learn: live mapping ─────────────────────────────────────────
        // Apply the same absolute/relative logic as the fixed CC mappings so that
        // relative encoders accumulate correctly instead of jumping to fixed positions.
        {
            juce::String learnedAddress;
            float        oscValue = 0.0f;
            bool         skip     = false;

            {
                const juce::ScopedLock lock (midiLearnLock);
                for (auto& [addr, b] : learnMap.bindings)
                {
                    if (b.cc != controllerNumber) continue;

                    if (b.mode == LearnMode::absolute)
                    {
                        oscValue = controllerValue / 127.0f;
                    }
                    else
                    {
                        // Relative mode: use the global encoder decoder + step size
                        auto delta = relativeControllerDelta (controllerValue, encoderMode);
                        if (! delta.has_value()) { skip = true; break; }
                        oscValue = juce::jlimit (0.0f, 1.0f,
                                                 b.currentValue
                                                     + ((*delta * encoderStep) / 127.0f));
                    }
                    b.currentValue = oscValue;
                    learnedAddress = addr;
                    break;
                }
            }

            if (skip) return;
            if (learnedAddress.isNotEmpty())
            {
                sendOscMessage (juce::OSCMessage (learnedAddress, oscValue));
                appendMidiLogAsync ("CC" + juce::String (controllerNumber)
                                   + " (learned) -> " + learnedAddress
                                   + " = " + juce::String (oscValue, 3));
                return;
            }
        }

    }

    bool handleBankSelectMessage (const juce::String& deviceName, int controllerNumber,
                                  int controllerValue)
    {
        if (controllerNumber != 0 && controllerNumber != 32)
            return false;

        std::optional<int> previousBank;
        int currentBank = 0;

        {
            const juce::ScopedLock lock (stateLock);

            if (controllerNumber == 0)
                bankSelectMsb = controllerValue;
            else
                bankSelectLsb = controllerValue;

            currentBank = (bankSelectMsb * 128) + bankSelectLsb;
            previousBank = lastBankSelect;
            lastBankSelect = currentBank;
        }

        const auto controllerName = controllerNumber == 0 ? "Bank Select MSB" : "Bank Select LSB";

        if (! previousBank.has_value())
        {
            appendMidiLogAsync ("MIDI " + juce::String (controllerName)
                               + " from " + deviceName
                               + ": baseline bank=" + juce::String (currentBank));
            return true;
        }

        if (currentBank == previousBank.value())
            return true;

        auto& button = currentBank > previousBank.value() ? nextBankButton : previousBankButton;
        clickPatchNavigationButtonAsync (
            button,
            juce::String (controllerName)
                + " " + juce::String (previousBank.value()) + " -> " + juce::String (currentBank));

        return true;
    }

    void handleProgramChangeMessage (const juce::String& deviceName, int programNumber)
    {
        std::optional<int> previousProgram;

        {
            const juce::ScopedLock lock (stateLock);
            previousProgram = lastProgramChange;
            lastProgramChange = programNumber;
        }

        if (! previousProgram.has_value())
        {
            appendMidiLogAsync ("MIDI Program Change from " + deviceName
                               + ": baseline program=" + juce::String (programNumber));
            return;
        }

        if (programNumber == previousProgram.value())
            return;

        auto& button = programNumber > previousProgram.value() ? nextProgramButton
                                                               : previousProgramButton;
        clickPatchNavigationButtonAsync (
            button,
            "Program Change " + juce::String (previousProgram.value()) + " -> "
                + juce::String (programNumber));
    }

    void clickPatchNavigationButtonAsync (juce::Button& button, juce::String sourceDescription)
    {
        juce::MessageManager::callAsync ([this, &button, sourceDescriptionText = std::move (sourceDescription)]
        {
            appendLog ("MIDI " + sourceDescriptionText + " -> virtual click: " + button.getButtonText());
            button.triggerClick();
        });
    }

    void sendSurgeNoteMessage (int noteNumber, int velocity)
    {
        sendOscMessage (juce::OSCMessage ("/mnote", (float) noteNumber, (float) velocity));
    }

    void triggerPatchNavigationAction (const char* actionName, const char* oscAddress)
    {
        sendOscMessage (juce::OSCMessage (oscAddress));
        appendLog ("Patch navigation: " + juce::String (actionName)
                   + " -> " + juce::String (oscAddress));
    }

    void randomizeAllParams()
    {
        juce::StringArray addresses;
        {
            const juce::ScopedLock lock (midiLearnLock);
            addresses = discoveredParamAddresses;
        }

        if (addresses.isEmpty())
        {
            appendLog ("No parameters discovered — use Query in MIDI Learn tab first");
            return;
        }

        // Params to leave at their current value (no universally correct target).
        static const juce::StringArray skipSuffixes {
            "pan",
            "volume",
        };

        // Params that must be actively reset to a fixed value every randomize.
        // "skip" would leave them stuck if they were ever randomized to a bad value.
        static const std::initializer_list<std::pair<const char*, float>> resetSuffixes {
            { "keytrack", 1.0f },  // off = all keys same pitch
            { "mute",     0.0f },  // muted oscillator = silence
            { "solo",     0.0f },  // soloed oscillator silences others
        };

        // Params that are safe to randomize but need a bounded range to avoid
        // clipping (ceiling) or inaudibility (floor). {suffix, min, max}.
        static const std::initializer_list<std::tuple<const char*, float, float>> clampedSuffixes {
            { "resonance",      0.0f, 0.70f },  // above ~0.7 risks self-oscillation
            { "drive",          0.0f, 0.40f },  // waveshaper drive clips hard at high values
            { "prefilter_gain", 0.0f, 0.50f },  // pre-filter boost drives everything into clipping
            { "fm_depth",       0.0f, 0.30f },  // high FM depth causes huge amplitude spikes
            { "level",          0.3f, 1.00f },  // send/aux levels — floor keeps patch audible
        };

        juce::Random rng;
        int skipped = 0, reset = 0, clamped = 0;
        for (const auto& addr : addresses)
        {
            const auto leaf = addr.fromLastOccurrenceOf ("/", false, false);
            if (skipSuffixes.contains (leaf)) { ++skipped; continue; }

            bool handled = false;
            for (const auto& [suffix, value] : resetSuffixes)
            {
                if (leaf == suffix)
                {
                    sendOscMessage (juce::OSCMessage (addr, value));
                    ++reset; handled = true; break;
                }
            }
            if (! handled)
            {
                for (const auto& [suffix, lo, hi] : clampedSuffixes)
                {
                    if (leaf == suffix)
                    {
                        sendOscMessage (juce::OSCMessage (addr, lo + rng.nextFloat() * (hi - lo)));
                        ++clamped; handled = true; break;
                    }
                }
            }
            if (! handled)
                sendOscMessage (juce::OSCMessage (addr, rng.nextFloat()));
        }

        appendLog ("Randomized " + juce::String (addresses.size() - skipped - reset - clamped)
                   + " free, " + juce::String (clamped) + " clamped, "
                   + juce::String (reset) + " reset, " + juce::String (skipped) + " skipped");
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

    // ── OSC receiver callback (message thread) ────────────────────────────────
    void oscMessageReceived (const juce::OSCMessage& message) override
    {
        const auto addr = message.getAddressPattern().toString();

        // Only collect base /param/... addresses — skip extended options and /doc/...
        if (! addr.startsWith ("/param/")) return;
        static const juce::StringArray extSuffixes {
            "abs", "enable", "tempo_sync", "extend", "deform",
            "const_rate", "gliss", "retrig", "curve"
        };
        if (extSuffixes.contains (addr.fromLastOccurrenceOf ("/", false, false))) return;

        {
            const juce::ScopedLock lock (midiLearnLock);
            if (discoveredParamAddresses.contains (addr)) return;
            discoveredParamAddresses.add (addr);
        }

        // Debounce: refresh the table 400 ms after the last new address arrives
        paramListDirty = true;
        startTimer (400);
    }

    // Fires on the message thread after the param-dump flood settles
    void timerCallback() override
    {
        stopTimer();
        if (! paramListDirty) return;
        paramListDirty = false;
        {
            const juce::ScopedLock lock (midiLearnLock);
            discoveredParamAddresses.sort (false);
        }
        midiLearnPanel.refresh();
        appendLog ("MIDI Learn: discovered "
                   + juce::String (discoveredParamAddresses.size())
                   + " addressable OSC parameters");
    }

    void saveMidiLearnMap()
    {
        // Must be called on the message thread (PropertiesFile is not thread-safe)
        juce::var v;
        {
            const juce::ScopedLock lock (midiLearnLock);
            v = learnMap.toVar();
        }
        settingsFile.setValue ("midiLearnMap", juce::JSON::toString (v, false));
        settingsFile.saveIfNeeded();
    }

    void loadMidiLearnMap()
    {
        const auto json = settingsFile.getValue ("midiLearnMap", "{}");
        juce::var v;
        if (juce::JSON::parse (json, v).wasOk())
        {
            const juce::ScopedLock lock (midiLearnLock);
            learnMap.fromVar (v);
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
        settingsFile.setValue ("voiceBackend", voiceBackendBox.getSelectedId());
        settingsFile.setValue ("voiceModel", voiceModelBox.getSelectedId());
        settingsFile.saveIfNeeded();
    }

    void updateEncoderModeFromComboBox()
    {
        {
            const juce::ScopedLock lock (stateLock);
            encoderMode = encoderModeFromId (encoderModeBox.getSelectedId());
        }

        saveSettings();
        appendLog ("Encoder mode set to " + encoderModeName (encoderMode));
    }

    void updateEncoderStepFromComboBox()
    {
        {
            const juce::ScopedLock lock (stateLock);
            encoderStep = juce::jlimit (1, 32, encoderStepBox.getSelectedId());
        }

        saveSettings();
        appendLog ("Encoder step set to " + juce::String (encoderStep));
    }

    void appendLog (const juce::String& line)
    {
        const auto timestamp = juce::Time::getCurrentTime().formatted ("%H:%M:%S");
        debugLogBox.moveCaretToEnd();
        debugLogBox.insertTextAtCaret ("[" + timestamp + "] " + line + "\n");
    }

    void appendLogAsync (juce::String line)
    {
        juce::MessageManager::callAsync ([this, logLine = std::move (line)]
        {
            appendLog (logLine);
        });
    }

    void appendMidiLog (const juce::String& line)
    {
        const auto timestamp = juce::Time::getCurrentTime().formatted ("%H:%M:%S");
        logBox.moveCaretToEnd();
        logBox.insertTextAtCaret ("[" + timestamp + "] " + line + "\n");
    }

    void appendMidiLogAsync (juce::String line)
    {
        juce::MessageManager::callAsync ([this, logLine = std::move (line)]
        {
            appendMidiLog (logLine);
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
    juce::Label patchNavigationLabel;
    juce::TextButton previousBankButton;
    juce::TextButton nextBankButton;
    juce::TextButton previousProgramButton;
    juce::TextButton nextProgramButton;
    juce::TextButton randomPatchButton;
    juce::TextButton randomParamsButton;
    juce::Label voiceLabel;
    juce::ComboBox voiceBackendBox;
    juce::ComboBox voiceModelBox;
    juce::TextButton voiceToggleButton;
    juce::Label voiceStatusLabel;
    juce::TextEditor voiceRecognizedBox;
    juce::Label statusLabel;
    juce::Label midiInputsLabel;
    juce::TextEditor midiInputsBox;
    juce::TabbedComponent logTabs { juce::TabbedButtonBar::TabsAtTop };
    juce::TextEditor logBox;
    juce::TextEditor debugLogBox;
    juce::TextEditor transcriptBox;
    juce::PropertiesFile& settingsFile;

    juce::CriticalSection oscLock;
    juce::CriticalSection activeNotesLock;
    juce::CriticalSection stateLock;
    juce::OSCSender oscSender;
    juce::AudioDeviceManager audioDeviceManager;
    juce::String targetHost = defaultTargetAddress;
    int targetPort = defaultOscPort;
    EncoderMode encoderMode = EncoderMode::absolute;
    int encoderStep = defaultEncoderStep;
    int bankSelectMsb = 0;
    int bankSelectLsb = 0;
    std::optional<int> lastBankSelect;
    std::optional<int> lastProgramChange;
    std::atomic<bool> voiceEnabled { false };
    std::atomic<bool> voiceWorkerShouldStop { false };
    std::thread voiceWorker;
    std::mutex voiceMutex;
    std::condition_variable voiceCv;
    std::vector<float> micInputBuffer;
    double micSampleRate = whisperSampleRate;
    std::mutex voiceCommandMutex;
    juce::String lastVoiceCommandNormalized;
    std::vector<std::unique_ptr<juce::MidiInput>> midiInputs;
    std::set<int> activeNotes;
    // ── MIDI Learn ────────────────────────────────────────────────────────────
    // Declared in this order: discoveredParamAddresses and learnMap must be
    // constructed before midiLearnPanel (which holds references to both).
    juce::CriticalSection  midiLearnLock;
    juce::OSCReceiver      oscReceiver;
    juce::StringArray      discoveredParamAddresses;
    MidiLearnMap           learnMap;
    std::atomic<int>       learningRow { -1 };
    bool                   paramListDirty = false;
    MidiLearnPanel         midiLearnPanel { discoveredParamAddresses, learnMap };

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
            setSize (920, 820);
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
