#include <juce_audio_plugin_client/detail/juce_IncludeSystemHeaders.h>
#include <juce_audio_plugin_client/detail/juce_IncludeModuleHeaders.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_opengl/juce_opengl.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include <unistd.h>

#if JucePlugin_Build_Standalone && JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP && defined(__QNXNTO__)

namespace
{
juce::String getRequestedRendererMode()
{
    auto mode = juce::SystemStats::getEnvironmentVariable ("SURGE_QNX_RENDERER", "opengl")
                    .trim()
                    .toLowerCase();

    if (mode != "software" && mode != "opengl")
        mode = "opengl";

    return mode;
}

std::unique_ptr<juce::FileLogger> createAppLogger()
{
    auto logFile = juce::File::getSpecialLocation (juce::File::currentApplicationFile)
                       .getParentDirectory()
                       .getChildFile ("SurgeXT.log");

    return std::make_unique<juce::FileLogger> (logFile,
                                               "Surge XT QNX standalone log",
                                               1024 * 1024);
}

juce::Rectangle<int> getPrimaryDisplayArea()
{
    if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    {
        const auto logicalBounds = display->logicalBounds.getSmallestIntegerContainer();

        if (! logicalBounds.isEmpty())
            return logicalBounds;
    }

    return { 0, 0, 1280, 1024 };
}

void applyPseudoFullscreenBounds (juce::ResizableWindow& window)
{
    const auto displayArea = getPrimaryDisplayArea();
    const auto currentBounds = window.getBounds();

    if (currentBounds != displayArea)
    {
        juce::Logger::writeToLog ("Applying QNX pseudo-fullscreen bounds: " + displayArea.toString()
                                  + " (was " + currentBounds.toString() + ")");
        window.setBounds (displayArea);
    }
}
}

namespace juce
{
class QnxStandaloneFilterWindow final : public StandaloneFilterWindow,
                                        private OpenGLRenderer,
                                        private Timer
{
public:
    QnxStandaloneFilterWindow (const String& title,
                               Colour backgroundColour,
                               std::unique_ptr<StandalonePluginHolder> pluginHolderIn,
                               bool shouldEnableOpenGLIn)
        : StandaloneFilterWindow (title, backgroundColour, std::move (pluginHolderIn)),
          shouldEnableOpenGL (shouldEnableOpenGLIn)
    {
        setFullScreen (true);
        applyPseudoFullscreenBounds (*this);

        if (shouldEnableOpenGL)
        {
            startTimerHz (30);
            tryAttachOpenGLIfReady();
        }
        else
        {
            Logger::writeToLog ("OpenGL renderer disabled for Surge standalone");
        }
    }

    ~QnxStandaloneFilterWindow() override
    {
        shutdownOpenGL();
    }

    void resized() override
    {
        StandaloneFilterWindow::resized();
        applyPseudoFullscreenBounds (*this);
        tryAttachOpenGLIfReady();
    }

    void visibilityChanged() override
    {
        StandaloneFilterWindow::visibilityChanged();
        tryAttachOpenGLIfReady();
    }

    void parentHierarchyChanged() override
    {
        StandaloneFilterWindow::parentHierarchyChanged();
        tryAttachOpenGLIfReady();
    }

private:
    void newOpenGLContextCreated() override
    {
        isOpenGLActive = true;
        Logger::writeToLog ("OpenGL context created for Surge standalone");
        requestVisualRefresh();
    }

    void renderOpenGL() override {}

    void openGLContextClosing() override
    {
        isOpenGLActive = false;
        Logger::writeToLog ("OpenGL context closing for Surge standalone");
    }

    void timerCallback() override
    {
        if (isOpenGLActive.load())
        {
            requestVisualRefresh();
            return;
        }

        tryAttachOpenGLIfReady();

        if (openGLAttachAttempted && ! isOpenGLActive.load())
        {
            const auto elapsedMs = Time::getMillisecondCounterHiRes() - openGLAttachStartMs;

            if (elapsedMs > 1500.0)
            {
                Logger::writeToLog ("OpenGL context was not created within 1500ms; falling back to software renderer");
                shutdownOpenGL();
                requestVisualRefresh();
                stopTimer();
            }
        }
    }

    void tryAttachOpenGLIfReady()
    {
        if (! shouldEnableOpenGL || openGLAttachAttempted || isOpenGLActive.load())
            return;

        if (getPeer() == nullptr || ! isShowing() || getWidth() <= 0 || getHeight() <= 0)
            return;

        openGLAttachAttempted = true;
        openGLAttachStartMs = Time::getMillisecondCounterHiRes();
        openGLContext.setRenderer (this);
        openGLContext.setComponentPaintingEnabled (true);
        openGLContext.setContinuousRepainting (true);
        openGLContext.attachTo (*this);
        Logger::writeToLog ("Requested OpenGL context attachment for Surge standalone");
    }

    void shutdownOpenGL()
    {
        openGLContext.detach();
        openGLContext.setRenderer (nullptr);
        isOpenGLActive = false;
        openGLAttachAttempted = false;
        shouldEnableOpenGL = false;
    }

    void requestVisualRefresh()
    {
        if (auto* content = getContentComponent())
            content->repaint();

        repaint();
    }

    OpenGLContext openGLContext;
    std::atomic<bool> isOpenGLActive { false };
    bool shouldEnableOpenGL = false;
    bool openGLAttachAttempted = false;
    double openGLAttachStartMs = 0.0;
};

class SurgeQnxStandaloneApp final : public JUCEApplication
{
public:
    SurgeQnxStandaloneApp()
    {
        PropertiesFile::Options options;
        options.applicationName = CharPointer_UTF8 (JucePlugin_Name);
        options.filenameSuffix = ".settings";
        options.folderName = "";
        options.osxLibrarySubFolder = "Application Support";
        appProperties.setStorageParameters (options);
    }

    const String getApplicationName() override            { return CharPointer_UTF8 (JucePlugin_Name); }
    const String getApplicationVersion() override         { return JucePlugin_VersionString; }
    bool moreThanOneInstanceAllowed() override            { return true; }
    void anotherInstanceStarted (const String&) override  {}

    std::unique_ptr<StandalonePluginHolder> createPluginHolder()
    {
       #ifdef JucePlugin_PreferredChannelConfigurations
        constexpr StandalonePluginHolder::PluginInOuts channels[] { JucePlugin_PreferredChannelConfigurations };
        const Array<StandalonePluginHolder::PluginInOuts> channelConfig (channels, numElementsInArray (channels));
       #else
        const Array<StandalonePluginHolder::PluginInOuts> channelConfig;
       #endif

        Logger::writeToLog ("Creating StandalonePluginHolder");

        return std::make_unique<StandalonePluginHolder> (appProperties.getUserSettings(),
                                                         false,
                                                         String{},
                                                         nullptr,
                                                         channelConfig,
                                                         false);
    }

    StandaloneFilterWindow* createWindow()
    {
        const auto& displays = Desktop::getInstance().getDisplays().displays;
        const auto rendererMode = getRequestedRendererMode();
        Logger::writeToLog ("Display count: " + String (displays.size()));

        if (displays.isEmpty())
        {
            Logger::writeToLog ("No displays available; window creation aborted");
            jassertfalse;
            return nullptr;
        }

        Logger::writeToLog ("Primary display area: "
                            + displays.getReference (0).logicalBounds.toString());
        Logger::writeToLog ("JUCE_QNX_ENABLE_OPENGL="
                            + String (std::getenv ("JUCE_QNX_ENABLE_OPENGL") != nullptr
                                          ? std::getenv ("JUCE_QNX_ENABLE_OPENGL")
                                          : "<unset>"));
        Logger::writeToLog ("JUCE_QNX_EMBEDDED_FULLSCREEN="
                            + String (std::getenv ("JUCE_QNX_EMBEDDED_FULLSCREEN") != nullptr
                                          ? std::getenv ("JUCE_QNX_EMBEDDED_FULLSCREEN")
                                          : "<unset>"));

        auto* window = new QnxStandaloneFilterWindow (getApplicationName(),
                                                      LookAndFeel::getDefaultLookAndFeel().findColour (ResizableWindow::backgroundColourId),
                                                      createPluginHolder(),
                                                      rendererMode == "opengl");

        Logger::writeToLog ("QNX standalone window created in pseudo-fullscreen mode");

        Logger::writeToLog ("StandaloneFilterWindow created");
        return window;
    }

    void initialise (const String&) override
    {
        const auto rendererMode = getRequestedRendererMode();
        setenv ("JUCE_QNX_ENABLE_OPENGL", rendererMode == "software" ? "0" : "1", 1);
        setenv ("JUCE_QNX_EMBEDDED_FULLSCREEN", "0", 1);

        logger = createAppLogger();
        Logger::setCurrentLogger (logger.get());

        Logger::writeToLog ("Application initialise()");
        Logger::writeToLog ("Process PID: " + String ((int) getpid()));
        Logger::writeToLog ("Requested renderer mode: " + rendererMode);

        mainWindow.reset (createWindow());

        if (mainWindow != nullptr)
        {
            Logger::writeToLog ("Setting main window visible");
            mainWindow->setVisible (true);
            mainWindow->toFront (true);
            Logger::writeToLog ("Main window visible");
        }
        else
        {
            Logger::writeToLog ("Main window was null; creating plugin holder without UI");
            pluginHolder = createPluginHolder();
        }
    }

    void shutdown() override
    {
        Logger::writeToLog ("Application shutdown()");
        pluginHolder = nullptr;
        mainWindow = nullptr;
        appProperties.saveIfNeeded();
        Logger::setCurrentLogger (nullptr);
        logger.reset();
    }

    void systemRequestedQuit() override
    {
        Logger::writeToLog ("systemRequestedQuit()");

        if (pluginHolder != nullptr)
            pluginHolder->savePluginState();

        if (mainWindow != nullptr)
            mainWindow->pluginHolder->savePluginState();

        if (ModalComponentManager::getInstance()->cancelAllModalComponents())
        {
            Timer::callAfterDelay (100, []()
            {
                if (auto* app = JUCEApplicationBase::getInstance())
                    app->systemRequestedQuit();
            });
        }
        else
        {
            quit();
        }
    }

private:
    ApplicationProperties appProperties;
    std::unique_ptr<FileLogger> logger;
    std::unique_ptr<StandaloneFilterWindow> mainWindow;
    std::unique_ptr<StandalonePluginHolder> pluginHolder;
};
}

juce::JUCEApplicationBase* juce_CreateApplication()
{
    return new juce::SurgeQnxStandaloneApp();
}

#endif
