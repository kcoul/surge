#include <juce_audio_plugin_client/detail/juce_IncludeSystemHeaders.h>
#include <juce_audio_plugin_client/detail/juce_IncludeModuleHeaders.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_opengl/juce_opengl.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include <algorithm>
#include <unistd.h>

#if JucePlugin_Build_Standalone && JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP && defined(__QNXNTO__)

namespace
{
constexpr const char* qnxOpenGLPresentationProperty = "juce_qnx_use_opengl_presentation";

juce::String getQnxBuildStamp()
{
    return juce::String (__DATE__) + " " + juce::String (__TIME__);
}

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

void applyDisplayBounds (juce::ResizableWindow& window)
{
    const auto displayArea = getPrimaryDisplayArea();
    const auto currentBounds = window.getBounds();

    if (currentBounds != displayArea)
    {
        juce::Logger::writeToLog ("Applying QNX display bounds: " + displayArea.toString()
                                  + " (was " + currentBounds.toString() + ")");
        window.setBounds (displayArea);
    }
}

}

namespace juce
{
String describeDesktopComponent (Component& component)
{
    const auto name = component.getName().isNotEmpty() ? component.getName()
                     : component.getTitle().isNotEmpty() ? component.getTitle()
                     : String (typeid (component).name());

    return name + " [" + String (typeid (component).name()) + "]";
}

String describePeerForLogging (Component& component)
{
    String description = describeDesktopComponent (component);

    if (auto* peer = component.getPeer())
    {
        description += " peerFlags=" + String (peer->getStyleFlags());
        description += " peerId=" + String ((int) peer->getUniqueID());
    }
    else
    {
        description += " peer=<null>";
    }

    description += " visible=" + String (component.isShowing() ? "yes" : "no");
    description += " bounds=" + component.getBounds().toString();
    description += " opaque=" + String (component.isOpaque() ? "yes" : "no");
    return description;
}

bool shouldUseOpenGLForDesktopPeer (Component& component)
{
    if (! component.isOnDesktop())
        return false;

    if (static_cast<bool> (component.getProperties().getWithDefault (qnxOpenGLPresentationProperty, false)))
        return true;

    return false;
}

class QnxOpenGLPeerAttachment final : private OpenGLRenderer
{
public:
    explicit QnxOpenGLPeerAttachment (Component& componentIn)
        : component (&componentIn)
    {
        componentIn.getProperties().set (qnxOpenGLPresentationProperty, true);
        Logger::writeToLog ("Tracking OpenGL attachment for " + describePeerForLogging (componentIn));
    }

    ~QnxOpenGLPeerAttachment() override
    {
        if (component != nullptr)
            Logger::writeToLog ("Destroying OpenGL attachment tracker for " + describePeerForLogging (*component));
        else
            Logger::writeToLog ("Destroying OpenGL attachment tracker for stale component");

        shutdown();
    }

    bool isFor (Component& candidate) const noexcept
    {
        return component.getComponent() == &candidate;
    }

    bool isStale() const noexcept
    {
        return component == nullptr || component->getPeer() == nullptr;
    }

    String describeCurrentState() const
    {
        if (component == nullptr)
            return "<stale component>";

        return describePeerForLogging (*component)
             + " attachAttempted=" + String (attachAttempted ? "yes" : "no")
             + " glActive=" + String (isOpenGLActive.load() ? "yes" : "no");
    }

    void update()
    {
        if (component == nullptr)
        {
            Logger::writeToLog ("OpenGL attachment update skipped for stale component");
            shutdown();
            return;
        }

        if (component->getPeer() == nullptr || ! component->isShowing()
            || component->getWidth() <= 0 || component->getHeight() <= 0)
        {
            Logger::writeToLog ("OpenGL attachment waiting for ready peer: " + describeCurrentState());
            return;
        }

        if (! attachAttempted)
        {
            attachAttempted = true;
            attachStartMs = Time::getMillisecondCounterHiRes();
            openGLContext.setRenderer (this);
            openGLContext.setComponentPaintingEnabled (true);
            openGLContext.setContinuousRepainting (true);
            Logger::writeToLog ("Calling OpenGLContext::attachTo for " + describeCurrentState());
            openGLContext.attachTo (*component);
            Logger::writeToLog ("Requested OpenGL context attachment for "
                                + describeCurrentState());
            return;
        }

        if (! isOpenGLActive.load()
            && Time::getMillisecondCounterHiRes() - attachStartMs > 1500.0)
        {
            Logger::writeToLog ("OpenGL context was not created within 1500ms for "
                                + describeCurrentState()
                                + "; falling back to software renderer");
            component->getProperties().set (qnxOpenGLPresentationProperty, false);
            requestVisualRefresh();
            shutdown();
        }
    }

    void shutdown()
    {
        if (component != nullptr)
            Logger::writeToLog ("Shutting down OpenGL attachment for " + describeCurrentState());

        openGLContext.detach();
        openGLContext.setRenderer (nullptr);
        isOpenGLActive = false;
        attachAttempted = false;
        attachStartMs = 0.0;
    }

private:
    void newOpenGLContextCreated() override
    {
        isOpenGLActive = true;

        if (component != nullptr)
            Logger::writeToLog ("OpenGL context created for " + describeCurrentState());

        requestVisualRefresh();
    }

    void renderOpenGL() override {}

    void openGLContextClosing() override
    {
        isOpenGLActive = false;

        if (component != nullptr)
            Logger::writeToLog ("OpenGL context closing for " + describeCurrentState());
    }

    void requestVisualRefresh()
    {
        if (component == nullptr)
            return;

        component->repaint();

        if (auto* peer = component->getPeer())
        {
            Logger::writeToLog ("Requesting immediate repaint flush for " + describeCurrentState());
            peer->performAnyPendingRepaintsNow();
        }
    }

    Component::SafePointer<Component> component;
    OpenGLContext openGLContext;
    std::atomic<bool> isOpenGLActive { false };
    bool attachAttempted = false;
    double attachStartMs = 0.0;
};

class QnxOpenGLPeerManager final : private Timer
{
public:
    explicit QnxOpenGLPeerManager (bool enabledIn)
        : enabled (enabledIn)
    {
        if (enabled)
            startTimerHz (30);
    }

    ~QnxOpenGLPeerManager() override
    {
        stopTimer();
        attachments.clear();
    }

private:
    void timerCallback() override
    {
        if (! enabled)
            return;

        for (int i = 0; i < ComponentPeer::getNumPeers(); ++i)
            if (auto* peer = ComponentPeer::getPeer (i))
                classifyPeer (peer->getComponent());

        attachments.erase (std::remove_if (attachments.begin(),
                                           attachments.end(),
                                           [] (const auto& attachment)
                                           {
                                               if (! attachment->isStale())
                                                   return false;

                                               Logger::writeToLog ("Removing stale OpenGL attachment tracker");
                                               return true;
                                           }),
                           attachments.end());

        for (auto& attachment : attachments)
            attachment->update();
    }

    void classifyPeer (Component& component)
    {
        if (! component.isOnDesktop())
            return;

        const auto wantsOpenGL = shouldUseOpenGLForDesktopPeer (component);
        component.getProperties().set (qnxOpenGLPresentationProperty, wantsOpenGL);

        const auto alreadyAttached = std::any_of (attachments.begin(),
                                                  attachments.end(),
                                                  [&component] (const auto& attachment)
                                                  {
                                                      return attachment->isFor (component);
                                                  });

        if (! wantsOpenGL)
            return;

        if (! alreadyAttached)
        {
            Logger::writeToLog ("Creating OpenGL attachment tracker for " + describePeerForLogging (component));
            attachments.push_back (std::make_unique<QnxOpenGLPeerAttachment> (component));
        }
    }

    bool enabled = false;
    std::vector<std::unique_ptr<QnxOpenGLPeerAttachment>> attachments;
};

class QnxStandaloneFilterWindow final : public StandaloneFilterWindow
{
public:
    QnxStandaloneFilterWindow (const String& title,
                               Colour backgroundColour,
                               std::unique_ptr<StandalonePluginHolder> pluginHolderIn)
        : StandaloneFilterWindow (title, backgroundColour, std::move (pluginHolderIn))
    {
        getProperties().set (qnxOpenGLPresentationProperty, true);
        setUsingNativeTitleBar (false);
        setTitleBarButtonsRequired (DocumentWindow::closeButton, false);
        updateDisplayLayout();
    }

    void resized() override
    {
        StandaloneFilterWindow::resized();
        updateDisplayLayout();
    }

    void visibilityChanged() override
    {
        StandaloneFilterWindow::visibilityChanged();
        updateDisplayLayout();
    }

    void parentHierarchyChanged() override
    {
        StandaloneFilterWindow::parentHierarchyChanged();
        updateDisplayLayout();
    }

private:
    void updateDisplayLayout()
    {
        if (isUpdatingDisplayLayout)
            return;

        const juce::ScopedValueSetter<bool> guard (isUpdatingDisplayLayout, true);

        if (! isFullScreen())
        {
            Logger::writeToLog ("Marking QNX standalone window as fullscreen for layout");
            setFullScreen (true);
        }

        applyDisplayBounds (*this);
    }
    bool isUpdatingDisplayLayout = false;
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
        Logger::writeToLog ("QNX build stamp: " + getQnxBuildStamp());
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
                                                      createPluginHolder());

        Logger::writeToLog ("QNX standalone window created in fullscreen-display mode");

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
        Logger::writeToLog (rendererMode == "opengl"
                                ? "Using OpenGL for all QNX desktop peers"
                                : "Using software rendering for all QNX desktop peers");

        openGLPeerManager = std::make_unique<QnxOpenGLPeerManager> (rendererMode == "opengl");

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
        openGLPeerManager = nullptr;
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
    std::unique_ptr<QnxOpenGLPeerManager> openGLPeerManager;
};
}

juce::JUCEApplicationBase* juce_CreateApplication()
{
    return new juce::SurgeQnxStandaloneApp();
}

#endif
