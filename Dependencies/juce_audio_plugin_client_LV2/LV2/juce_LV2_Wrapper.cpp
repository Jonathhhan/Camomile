/*
  ==============================================================================

   Juce LV2 Wrapper

  ==============================================================================
*/


#define JUCE_GUI_BASICS_INCLUDE_XHEADERS 1
#include <juce_gui_basics/juce_gui_basics.h>

#include <juce_audio_plugin_client/utility/juce_CheckSettingMacros.h>
#include <juce_audio_plugin_client/utility/juce_IncludeModuleHeaders.h>
#include <juce_core/system/juce_TargetPlatform.h>

#if JucePlugin_Build_LV2

#if JUCE_LINUX
#include <X11/Xlib.h>
#define JUCE_GUI_BASICS_INCLUDE_XHEADERS 1

#include <juce_audio_plugin_client/utility/juce_FakeMouseMoveGenerator.h>
#include <juce_audio_plugin_client/utility/juce_WindowsHooks.h>
#undef KeyPress
#endif

#include <fstream>
#include <iostream>

// LV2 includes..
#include "lv2/core/lv2.h"
#include "lv2/atom/atom.h"
#include "lv2/atom/util.h"
#include "lv2/buf-size/buf-size.h"
#include "lv2/instance-access/instance-access.h"
#include "lv2/midi/midi.h"
#include "lv2/options/options.h"
#include "lv2/port-props/port-props.h"
#include "lv2/presets/presets.h"
#include "lv2/state/state.h"
#include "lv2/time/time.h"
#include "lv2/ui/ui.h"
#include "lv2/units/units.h"
#include "lv2/urid/urid.h"
#include "includes/lv2_external_ui.h"
#include "includes/lv2_programs.h"

#define JUCE_LV2_STATE_STRING_URI "urn:juce:stateString"
#define JUCE_LV2_STATE_BINARY_URI "urn:juce:stateBinary"

//==============================================================================
// Various helper functions for creating the ttl files

#if JUCE_MAC
#define PLUGIN_EXT ".dylib"
#elif JUCE_LINUX
#define PLUGIN_EXT ".so"
#elif JUCE_WINDOWS
#define PLUGIN_EXT ".dll"
#endif

using namespace juce;

//==============================================================================
#if JUCE_LINUX

struct SharedMessageThread  : public Thread
{
    SharedMessageThread()  : Thread ("Lv2MessageThread")
    {
        startThread (7);
        
        while (! initialised)
            sleep (1);
    }
    
    ~SharedMessageThread() override
    {
        signalThreadShouldExit();
        JUCEApplicationBase::quit();
        waitForThreadToExit (5000);
    }
    
    void run() override
    {
        ScopedJuceInitialiser_GUI juceInitialiser;
        initialised = true;
        
        MessageManager::getInstance()->setCurrentThreadAsMessageThread();
        
        XWindowSystem::getInstance();
        
        while ((! threadShouldExit()) && MessageManager::getInstance()->runDispatchLoopUntil (250))
        {}
    }
    
    bool initialised = false;
};

#endif

//==============================================================================
/**
 Juce LV2 External UI handle
 */
class JuceLv2ExternalUIWrapper : public LV2_External_UI_Widget
{
public:
    JuceLv2ExternalUIWrapper (AudioProcessorEditor* editor, LV2_External_UI_Host const* host, LV2UI_Controller controller, const String& title)
    : window (editor, host, controller, title)
    {
        // external UI calls
        run  = [](LV2_External_UI_Widget* widget)
        {
            const MessageManagerLock mmLock;
            static_cast<JuceLv2ExternalUIWrapper*>(widget)->window.repaint();
        };
        
        show = [](LV2_External_UI_Widget* widget)
        {
            const MessageManagerLock mmLock;
            static_cast<JuceLv2ExternalUIWrapper*>(widget)->window.setVisible(false);
        };
        
        hide = [](LV2_External_UI_Widget* widget)
        {
            const MessageManagerLock mmLock;
            static_cast<JuceLv2ExternalUIWrapper*>(widget)->window.setVisible(false);
        };
    }
    
    ~JuceLv2ExternalUIWrapper() = default;
    
    
private:
    class InternalWindow : public DocumentWindow
    {
    public:
        InternalWindow (AudioProcessorEditor* editor, LV2_External_UI_Host const* _host, LV2UI_Controller _controller, const String& title)
        : DocumentWindow (title, Colours::white, DocumentWindow::minimiseButton | DocumentWindow::closeButton, true)
        , host(_host)
        , controller(_controller)
        {
            setOpaque (true);
            setContentNonOwned (editor, true);
            setSize (editor->getWidth(), editor->getHeight());
            setUsingNativeTitleBar (true);
        }
        
        void closeButtonPressed() override
        {
            setVisible(false);
            host->ui_closed(controller);
        }
        
    private:
        LV2_External_UI_Host const* host;
        LV2UI_Controller controller;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InternalWindow);
    };
    
    InternalWindow window;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JuceLv2ExternalUIWrapper);
};

//==============================================================================
/**
 Juce LV2 Parent UI container, listens for resize events and passes them to ui-resize
 */
class JuceLv2ParentContainer : public Component
{
public:
    JuceLv2ParentContainer (AudioProcessorEditor* editor, const LV2UI_Resize* uiResize_)
    : uiResize(uiResize_)
    {
        setOpaque (true);
        editor->setOpaque (true);
        setBounds (editor->getBounds());
        
        editor->setTopLeftPosition (0, 0);
        addAndMakeVisible (editor);
    }
    
    ~JuceLv2ParentContainer()
    {
    }
    
    void paint (Graphics&) {}
    void paintOverChildren (Graphics&) {}
    
    void childBoundsChanged (Component* child)
    {
        const int cw = child->getWidth();
        const int ch = child->getHeight();
        
#if JUCE_LINUX
        X11Symbols::getInstance()->xResizeWindow (display, (Window) getWindowHandle(), cw, ch);
#else
        setSize (cw, ch);
#endif
        
        if (uiResize != nullptr)
            uiResize->ui_resize (uiResize->handle, cw, ch);
    }
    
    void reset (const LV2UI_Resize* uiResize_)
    {
        uiResize = uiResize_;
        
        if (uiResize != nullptr)
            uiResize->ui_resize (uiResize->handle, getWidth(), getHeight());
    }
    
private:
    //==============================================================================
    const LV2UI_Resize* uiResize;
#if JUCE_LINUX
    ::Display* display = XWindowSystem::getInstance()->getDisplay();
#endif
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JuceLv2ParentContainer);
};

//==============================================================================
/**
 Juce LV2 UI handle
 */
class JuceLv2UIWrapper : public AudioProcessorListener
{
public:
    JuceLv2UIWrapper (ThreadLocalValue<bool>& inParameterChangedCallback_, AudioProcessor* filter_, LV2UI_Write_Function writeFunction_, LV2UI_Controller controller_,
                      LV2UI_Widget* widget, const LV2_Feature* const* features, bool isExternal_)
    : inParameterChangedCallback(inParameterChangedCallback_),
    filter (filter_),
    writeFunction (writeFunction_),
    controller (controller_),
    isExternal (isExternal_),
    controlPortOffset (0),
    lastProgramCount (0),
    uiTouch (nullptr),
    programsHost (nullptr),
    externalUIHost (nullptr),
    lastExternalUIPos (-1, -1),
    uiResize (nullptr)
    {
        jassert (filter != nullptr);
        
        filter->addListener(this);
        
        if (filter->hasEditor())
        {
            editor = std::unique_ptr<AudioProcessorEditor>(filter->createEditorIfNeeded());
            
            if (editor == nullptr)
            {
                *widget = nullptr;
                return;
            }
        }
        
        for (int i = 0; features[i] != nullptr; ++i)
        {
            if (strcmp(features[i]->URI, LV2_UI__touch) == 0)
                uiTouch = (const LV2UI_Touch*)features[i]->data;
            
            else if (strcmp(features[i]->URI, LV2_PROGRAMS__Host) == 0)
                programsHost = (const LV2_Programs_Host*)features[i]->data;
        }
        
        if (isExternal)
        {
            resetExternalUI (features);
            
            if (externalUIHost != nullptr)
            {
                String title (filter->getName());
                
                if (externalUIHost->plugin_human_id != nullptr)
                    title = externalUIHost->plugin_human_id;
                
                externalUI = std::make_unique<JuceLv2ExternalUIWrapper>(editor.get(), externalUIHost, controller_, title);
                *widget = externalUI.get();
            }
            else
            {
                *widget = nullptr;
            }
        }
        else
        {
            resetParentUI (features);
            
            if (parentContainer != nullptr)
                *widget = parentContainer->getWindowHandle();
            else
                *widget = nullptr;
        }
        
        controlPortOffset += 1; // Time position and MIDI in port
        if(filter->producesMidi())
        {
            controlPortOffset += 1;
        }
        controlPortOffset += 1; // Freewheel port
        controlPortOffset += 1; // Lantecy port
        controlPortOffset += filter->getTotalNumInputChannels();
        controlPortOffset += filter->getTotalNumOutputChannels();
        
        lastProgramCount = filter->getNumPrograms();
    }
    
    ~JuceLv2UIWrapper()
    {
        PopupMenu::dismissAllActiveMenus();
        
        filter->removeListener(this);
        
        parentContainer = nullptr;
        externalUI = nullptr;
        externalUIHost = nullptr;
        
        if (editor != nullptr)
        {
            filter->editorBeingDeleted (editor.get());
            editor = nullptr;
        }
    }
    
    //==============================================================================
    // LV2 core calls
    
    void lv2Cleanup()
    {
        const MessageManagerLock mmLock;
        
        if (isExternal)
        {
            externalUIHost = nullptr;
            
            if (externalUI != nullptr)
            {
                externalUI->hide(externalUI.get());
            }
        }
        else
        {
            if (parentContainer)
            {
                parentContainer->setVisible (false);
                if (parentContainer->isOnDesktop())
                    parentContainer->removeFromDesktop();
            }
        }
    }
    
    //==============================================================================
    // Juce calls
    
    void audioProcessorParameterChanged (AudioProcessor*, int index, float newValue)
    {
        if (inParameterChangedCallback.get())
        {
            inParameterChangedCallback = false;
            return;
        }
        if(auto* rangeParam = dynamic_cast<RangedAudioParameter*>(filter->getParameters()[index]))
            newValue = rangeParam->convertFrom0to1(newValue);
        if (writeFunction != nullptr && controller != nullptr)
            writeFunction (controller, index + controlPortOffset, sizeof (float), 0, &newValue);
    }
    
    void audioProcessorChanged (AudioProcessor*)
    {
        if (filter != nullptr && programsHost != nullptr)
        {
            if (filter->getNumPrograms() != lastProgramCount)
            {
                programsHost->program_changed (programsHost->handle, -1);
                lastProgramCount = filter->getNumPrograms();
            }
            else
                programsHost->program_changed (programsHost->handle, filter->getCurrentProgram());
        }
    }
    
    void audioProcessorParameterChangeGestureBegin (AudioProcessor*, int parameterIndex)
    {
        if (uiTouch != nullptr)
            uiTouch->touch (uiTouch->handle, parameterIndex + controlPortOffset, true);
    }
    
    void audioProcessorParameterChangeGestureEnd (AudioProcessor*, int parameterIndex)
    {
        if (uiTouch != nullptr)
            uiTouch->touch (uiTouch->handle, parameterIndex + controlPortOffset, false);
    }
    
    //==============================================================================
    void resetIfNeeded (LV2UI_Write_Function writeFunction_, LV2UI_Controller controller_, LV2UI_Widget* widget,
                        const LV2_Feature* const* features)
    {
        writeFunction = writeFunction_;
        controller = controller_;
        uiTouch = nullptr;
        programsHost = nullptr;
        
        for (int i = 0; features[i] != nullptr; ++i)
        {
            if (strcmp(features[i]->URI, LV2_UI__touch) == 0)
                uiTouch = (const LV2UI_Touch*)features[i]->data;
            
            else if (strcmp(features[i]->URI, LV2_PROGRAMS__Host) == 0)
                programsHost = (const LV2_Programs_Host*)features[i]->data;
        }
        
        if (isExternal)
        {
            resetExternalUI (features);
            *widget = externalUI.get();
        }
        else
        {
            resetParentUI (features);
            *widget = parentContainer->getWindowHandle();
        }
    }
    
    void repaint()
    {
        {
            const MessageManagerLock mmLock;
            
            if (editor != nullptr)
                editor->repaint();
            
            if (parentContainer != nullptr)
                parentContainer->repaint();
        }
        
        if (externalUI != nullptr)
            externalUI->run(externalUI.get());
    }
    
private:
    ThreadLocalValue<bool>& inParameterChangedCallback;
    AudioProcessor* const filter;
    std::unique_ptr<AudioProcessorEditor> editor;
    
    LV2UI_Write_Function writeFunction;
    LV2UI_Controller controller;
    const bool isExternal;
    
    uint32 controlPortOffset;
    int lastProgramCount;
    
    const LV2UI_Touch* uiTouch;
    const LV2_Programs_Host* programsHost;
    
    std::unique_ptr<JuceLv2ExternalUIWrapper> externalUI;
    const LV2_External_UI_Host* externalUIHost;
    Point<int> lastExternalUIPos;
    
    std::unique_ptr<JuceLv2ParentContainer> parentContainer;
    const LV2UI_Resize* uiResize;
    
#if JUCE_LINUX
    ::Display* display = XWindowSystem::getInstance()->getDisplay();
#endif
    
    //==============================================================================
    void resetExternalUI (const LV2_Feature* const* features)
    {
        externalUIHost = nullptr;
        
        for (int i = 0; features[i] != nullptr; ++i)
        {
            if (strcmp(features[i]->URI, LV2_EXTERNAL_UI__Host) == 0)
            {
                externalUIHost = (const LV2_External_UI_Host*)features[i]->data;
                break;
            }
        }
    }
    
    void resetParentUI (const LV2_Feature* const* features)
    {
        void* parent = nullptr;
        uiResize = nullptr;
        
        for (int i = 0; features[i] != nullptr; ++i)
        {
            if (strcmp(features[i]->URI, LV2_UI__parent) == 0)
                parent = features[i]->data;
            
            else if (strcmp(features[i]->URI, LV2_UI__resize) == 0)
                uiResize = (const LV2UI_Resize*)features[i]->data;
        }
        
        if (parent != nullptr)
        {
            if (parentContainer == nullptr)
                parentContainer = std::make_unique<JuceLv2ParentContainer>(editor.get(), uiResize);
            
            parentContainer->setVisible (false);
            
            if (parentContainer->isOnDesktop())
                parentContainer->removeFromDesktop();
            
            parentContainer->addToDesktop (0, parent);
            
#if JUCE_LINUX
            Window hostWindow = (Window) parent;
            Window editorWnd  = (Window) parentContainer->getWindowHandle();
            X11Symbols::getInstance()->xReparentWindow (display, editorWnd, hostWindow, 0, 0);
#endif
            
            parentContainer->reset (uiResize);
            parentContainer->setVisible (true);
        }
    }
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JuceLv2UIWrapper)
};

//==============================================================================
/**
 Juce LV2 handle
 */
class JuceLv2Wrapper : public AudioPlayHead
{
public:
    //==============================================================================
    JuceLv2Wrapper (std::unique_ptr<AudioProcessor> processor, double sampleRate_, const LV2_Feature* const* features)
    : filter(std::move(processor)), bufferSize(2048), sampleRate(sampleRate_)
    {
        inParameterChangedCallback = false;
        
        auto const numInputChannels = filter->getTotalNumInputChannels();
        auto const numOutputChannels = filter->getTotalNumOutputChannels();
        filter->setPlayConfigDetails (numInputChannels, numOutputChannels, sampleRate, bufferSize);
        filter->setPlayHead (this);
        portAudioIns.resize(numInputChannels, nullptr);
        portAudioOuts.resize(numOutputChannels, nullptr);
        
        auto const numParameters = filter->getParameters().size();
        portControls.resize(numParameters, nullptr);
        
        curPosInfo.resetToDefault();
        
        // we need URID_Map first
        for (int i=0; features[i] != nullptr; ++i)
        {
            if (strcmp(features[i]->URI, LV2_URID__map) == 0)
            {
                uridMap = (const LV2_URID_Map*)features[i]->data;
                break;
            }
        }
        
        // we require uridMap to work properly (it's set as required feature)
        jassert (uridMap != nullptr);
        
        if (uridMap != nullptr)
        {
            uridAtomBlank = uridMap->map(uridMap->handle, LV2_ATOM__Blank);
            uridAtomObject = uridMap->map(uridMap->handle, LV2_ATOM__Object);
            uridAtomDouble = uridMap->map(uridMap->handle, LV2_ATOM__Double);
            uridAtomFloat = uridMap->map(uridMap->handle, LV2_ATOM__Float);
            uridAtomInt = uridMap->map(uridMap->handle, LV2_ATOM__Int);
            uridAtomLong = uridMap->map(uridMap->handle, LV2_ATOM__Long);
            uridAtomSequence = uridMap->map(uridMap->handle, LV2_ATOM__Sequence);
            uridMidiEvent = uridMap->map(uridMap->handle, LV2_MIDI__MidiEvent);
            uridTimePos = uridMap->map(uridMap->handle, LV2_TIME__Position);
            uridTimeBar = uridMap->map(uridMap->handle, LV2_TIME__bar);
            uridTimeBarBeat = uridMap->map(uridMap->handle, LV2_TIME__barBeat);
            uridTimeBeatsPerBar = uridMap->map(uridMap->handle, LV2_TIME__beatsPerBar);
            uridTimeBeatsPerMinute = uridMap->map(uridMap->handle, LV2_TIME__beatsPerMinute);
            uridTimeBeatUnit = uridMap->map(uridMap->handle, LV2_TIME__beatUnit);
            uridTimeFrame = uridMap->map(uridMap->handle, LV2_TIME__frame);
            uridTimeSpeed = uridMap->map(uridMap->handle, LV2_TIME__speed);
            
            for (int i=0; features[i] != nullptr; ++i)
            {
                if (strcmp(features[i]->URI, LV2_OPTIONS__options) == 0)
                {
                    const LV2_Options_Option* options = (const LV2_Options_Option*)features[i]->data;
                    
                    for (int j=0; options[j].key != 0; ++j)
                    {
                        if (options[j].key == uridMap->map(uridMap->handle, LV2_BUF_SIZE__nominalBlockLength))
                        {
                            if (options[j].type == uridAtomInt)
                            {
                                bufferSize = *(int*)options[j].value;
                                usingNominalBlockLength = true;
                            }
                            else
                            {
                                std::cerr << "Host provides nominalBlockLength but has wrong value type" << std::endl;
                            }
                            break;
                        }
                        
                        if (options[j].key == uridMap->map(uridMap->handle, LV2_BUF_SIZE__maxBlockLength))
                        {
                            if (options[j].type == uridAtomInt)
                                bufferSize = *(int*)options[j].value;
                            else
                                std::cerr << "Host provides maxBlockLength but has wrong value type" << std::endl;
                            
                            // no break, continue in case host supports nominalBlockLength
                        }
                    }
                    break;
                }
            }
        }
        
        progDesc.bank = 0;
        progDesc.program = 0;
        progDesc.name = nullptr;
    }
    
    ~JuceLv2Wrapper ()
    {
        const MessageManagerLock mmLock;
        ui = nullptr;
        filter = nullptr;
        
        if (progDesc.name != nullptr)
            free((void*)progDesc.name);
    }
    
    //==============================================================================
    // LV2 core calls
    
    float getParameterValue(int index)
    {
        auto* param = filter->getParameters()[index];
        if(auto* rangeParam = dynamic_cast<RangedAudioParameter*>(param))
            return rangeParam->convertFrom0to1(param->getValue());
        else if(param != nullptr)
            return param->getValue();
        else
            return 0.0;
    }
    
    const void setParameterValue(int index, float value)
    {
        auto* param = filter->getParameters()[index];
        if(auto* rangeParam = dynamic_cast<RangedAudioParameter*>(param))
        {
            auto const newValue = rangeParam->convertTo0to1(value);
            if(std::abs(newValue - rangeParam->getValue()) > std::numeric_limits<float>::epsilon())
            {
                rangeParam->setValue(newValue);
                inParameterChangedCallback = true;
                rangeParam->sendValueChangedMessageToListeners(newValue);
            }
        }
        else if (auto* param = filter->getParameters()[index])
        {
            if(std::abs(value - rangeParam->getValue()) > std::numeric_limits<float>::epsilon())
            {
                rangeParam->setValue(value);
                inParameterChangedCallback = true;
                rangeParam->sendValueChangedMessageToListeners(value);
            }
        }
    }
    
    bool isParameterAutomatable3(int index)
    {
        if (auto* param = filter->getParameters()[index])
            return param->isAutomatable();
        
        return true;
    }
    
    const String getParameterName3(int index)
    {
        if(auto* param = filter->getParameters()[index])
            return param->getName(1024);
        else
            return {};
    }
    
    void lv2ConnectPort (uint32 portId, void* dataLocation)
    {
        uint32 index = 0;
        
        if (portId == index++)
        {
            portEventsIn = static_cast<LV2_Atom_Sequence*>(dataLocation);
            return;
        }

        if (filter->producesMidi() && portId == index++)
        {
            portMidiOut = static_cast<LV2_Atom_Sequence*>(dataLocation);
            return;
        }
        
        if (portId == index++)
        {
            portFreewheel = static_cast<float*>(dataLocation);
            return;
        }
        
        if (portId == index++)
        {
            portLatency = static_cast<float*>(dataLocation);
            return;
        }
        
        auto const numInputChannels = filter->getTotalNumInputChannels();
        for (int i = 0; i < numInputChannels; ++i)
        {
            if (portId == index++)
            {
                portAudioIns[i] = static_cast<float*>(dataLocation);
                return;
            }
        }
        
        auto const numOutputChannels = filter->getTotalNumOutputChannels();
        for (int i = 0; i < numOutputChannels; ++i)
        {
            if (portId == index++)
            {
                portAudioOuts[i] = static_cast<float*>(dataLocation);
                return;
            }
        }
        
        auto const numParameters = filter->getParameters().size();
        for (int i = 0; i < numParameters; ++i)
        {
            if (portId == index++)
            {
                portControls[i] = static_cast<float*>(dataLocation);
                return;
            }
        }
    }
    
    void lv2Activate()
    {
        jassert (filter != nullptr);
        
        filter->prepareToPlay (sampleRate, bufferSize);
        filter->setPlayConfigDetails (filter->getTotalNumInputChannels(), filter->getTotalNumOutputChannels(), sampleRate, bufferSize);
        
        channels.calloc (filter->getTotalNumInputChannels() + filter->getTotalNumOutputChannels());
        if(filter->acceptsMidi() || filter->producesMidi())
        {
            midiEvents.ensureSize (2048);
            midiEvents.clear();
        }
    }
    
    void lv2Deactivate()
    {
        jassert (filter != nullptr);
        
        filter->releaseResources();
        
        channels.free();
    }
    
    void lv2Run (uint32 sampleCount)
    {
        jassert (filter != nullptr);
        
        if (portLatency != nullptr)
            *portLatency = filter->getLatencySamples();
        
        if (portFreewheel != nullptr)
            filter->setNonRealtime (*portFreewheel >= 0.5f);
        
        if (sampleCount == 0)
        {
            /**
             LV2 pre-roll
             Hosts might use this to force plugins to update its output control ports.
             (plugins can only access port locations during run) */
            return;
        }
        
        // Check for updated parameters
        for (int i = 0; i < portControls.size(); ++i)
        {
            if (portControls[i] != nullptr)
            {
                setParameterValue(i, *portControls[i]);
            }
        }
        
        {
            const ScopedLock sl (filter->getCallbackLock());
            
            if (filter->isSuspended() && false)
            {
                for (int i = 0; i < filter->getTotalNumOutputChannels(); ++i)
                    zeromem (portAudioOuts[i], sizeof (float) * sampleCount);
            }
            else
            {
                int i;
                for (i = 0; i < filter->getTotalNumOutputChannels(); ++i)
                {
                    channels[i] = portAudioOuts[i];
                    
                    if (i < filter->getTotalNumInputChannels() && portAudioIns[i] != portAudioOuts[i])
                        FloatVectorOperations::copy (portAudioOuts [i], portAudioIns[i], sampleCount);
                }
                
                for (; i < filter->getTotalNumInputChannels(); ++i)
                    channels [i] = portAudioIns[i];
                
                if (portEventsIn != nullptr)
                {
                    midiEvents.clear();
                    
                    LV2_ATOM_SEQUENCE_FOREACH(portEventsIn, iter)
                    {
                        const LV2_Atom_Event* event = (const LV2_Atom_Event*)iter;
                        
                        if (event == nullptr)
                            continue;
                        if (event->time.frames >= sampleCount)
                            break;
                        
                        if (filter->acceptsMidi() && event->body.type == uridMidiEvent)
                        {
                            const uint8* data = (const uint8*)(event + 1);
                            midiEvents.addEvent(data, event->body.size, static_cast<int>(event->time.frames));
                            continue;
                        }
                        
                        if (event->body.type == uridAtomBlank || event->body.type == uridAtomObject)
                        {
                            const LV2_Atom_Object* obj = (LV2_Atom_Object*)&event->body;
                            
                            if (obj->body.otype != uridTimePos)
                                continue;
                            
                            LV2_Atom* bar = nullptr;
                            LV2_Atom* barBeat = nullptr;
                            LV2_Atom* beatUnit = nullptr;
                            LV2_Atom* beatsPerBar = nullptr;
                            LV2_Atom* beatsPerMinute = nullptr;
                            LV2_Atom* frame = nullptr;
                            LV2_Atom* speed = nullptr;
                            
                            lv2_atom_object_get (obj,
                                                 uridTimeBar, &bar,
                                                 uridTimeBarBeat, &barBeat,
                                                 uridTimeBeatUnit, &beatUnit,
                                                 uridTimeBeatsPerBar, &beatsPerBar,
                                                 uridTimeBeatsPerMinute, &beatsPerMinute,
                                                 uridTimeFrame, &frame,
                                                 uridTimeSpeed, &speed,
                                                 nullptr);
                            
                            // need to handle this first as other values depend on it
                            if (speed != nullptr)
                            {
                                /**/ if (speed->type == uridAtomDouble)
                                    lastPositionData.speed = ((LV2_Atom_Double*)speed)->body;
                                else if (speed->type == uridAtomFloat)
                                    lastPositionData.speed = ((LV2_Atom_Float*)speed)->body;
                                else if (speed->type == uridAtomInt)
                                    lastPositionData.speed = ((LV2_Atom_Int*)speed)->body;
                                else if (speed->type == uridAtomLong)
                                    lastPositionData.speed = ((LV2_Atom_Long*)speed)->body;
                                
                                curPosInfo.isPlaying = lastPositionData.speed != 0.0;
                            }
                            
                            if (bar != nullptr)
                            {
                                /**/ if (bar->type == uridAtomDouble)
                                    lastPositionData.bar = ((LV2_Atom_Double*)bar)->body;
                                else if (bar->type == uridAtomFloat)
                                    lastPositionData.bar = ((LV2_Atom_Float*)bar)->body;
                                else if (bar->type == uridAtomInt)
                                    lastPositionData.bar = ((LV2_Atom_Int*)bar)->body;
                                else if (bar->type == uridAtomLong)
                                    lastPositionData.bar = ((LV2_Atom_Long*)bar)->body;
                            }
                            
                            if (barBeat != nullptr)
                            {
                                /**/ if (barBeat->type == uridAtomDouble)
                                    lastPositionData.barBeat = ((LV2_Atom_Double*)barBeat)->body;
                                else if (barBeat->type == uridAtomFloat)
                                    lastPositionData.barBeat = ((LV2_Atom_Float*)barBeat)->body;
                                else if (barBeat->type == uridAtomInt)
                                    lastPositionData.barBeat = ((LV2_Atom_Int*)barBeat)->body;
                                else if (barBeat->type == uridAtomLong)
                                    lastPositionData.barBeat = ((LV2_Atom_Long*)barBeat)->body;
                            }
                            
                            if (beatUnit != nullptr)
                            {
                                /**/ if (beatUnit->type == uridAtomDouble)
                                    lastPositionData.beatUnit = ((LV2_Atom_Double*)beatUnit)->body;
                                else if (beatUnit->type == uridAtomFloat)
                                    lastPositionData.beatUnit = ((LV2_Atom_Float*)beatUnit)->body;
                                else if (beatUnit->type == uridAtomInt)
                                    lastPositionData.beatUnit = ((LV2_Atom_Int*)beatUnit)->body;
                                else if (beatUnit->type == uridAtomLong)
                                    lastPositionData.beatUnit = static_cast<uint32_t>(((LV2_Atom_Long*)beatUnit)->body);
                                
                                if (lastPositionData.beatUnit > 0)
                                    curPosInfo.timeSigDenominator = lastPositionData.beatUnit;
                            }
                            
                            if (beatsPerBar != nullptr)
                            {
                                /**/ if (beatsPerBar->type == uridAtomDouble)
                                    lastPositionData.beatsPerBar = ((LV2_Atom_Double*)beatsPerBar)->body;
                                else if (beatsPerBar->type == uridAtomFloat)
                                    lastPositionData.beatsPerBar = ((LV2_Atom_Float*)beatsPerBar)->body;
                                else if (beatsPerBar->type == uridAtomInt)
                                    lastPositionData.beatsPerBar = ((LV2_Atom_Int*)beatsPerBar)->body;
                                else if (beatsPerBar->type == uridAtomLong)
                                    lastPositionData.beatsPerBar = ((LV2_Atom_Long*)beatsPerBar)->body;
                                
                                if (lastPositionData.beatsPerBar > 0.0f)
                                    curPosInfo.timeSigNumerator = lastPositionData.beatsPerBar;
                            }
                            
                            if (beatsPerMinute != nullptr)
                            {
                                /**/ if (beatsPerMinute->type == uridAtomDouble)
                                    lastPositionData.beatsPerMinute = ((LV2_Atom_Double*)beatsPerMinute)->body;
                                else if (beatsPerMinute->type == uridAtomFloat)
                                    lastPositionData.beatsPerMinute = ((LV2_Atom_Float*)beatsPerMinute)->body;
                                else if (beatsPerMinute->type == uridAtomInt)
                                    lastPositionData.beatsPerMinute = ((LV2_Atom_Int*)beatsPerMinute)->body;
                                else if (beatsPerMinute->type == uridAtomLong)
                                    lastPositionData.beatsPerMinute = ((LV2_Atom_Long*)beatsPerMinute)->body;
                                
                                if (lastPositionData.beatsPerMinute > 0.0f)
                                {
                                    curPosInfo.bpm = lastPositionData.beatsPerMinute;
                                    
                                    if (lastPositionData.speed != 0)
                                        curPosInfo.bpm *= std::abs(lastPositionData.speed);
                                }
                            }
                            
                            if (frame != nullptr)
                            {
                                /**/ if (frame->type == uridAtomDouble)
                                    lastPositionData.frame = ((LV2_Atom_Double*)frame)->body;
                                else if (frame->type == uridAtomFloat)
                                    lastPositionData.frame = ((LV2_Atom_Float*)frame)->body;
                                else if (frame->type == uridAtomInt)
                                    lastPositionData.frame = ((LV2_Atom_Int*)frame)->body;
                                else if (frame->type == uridAtomLong)
                                    lastPositionData.frame = ((LV2_Atom_Long*)frame)->body;
                                
                                if (lastPositionData.frame >= 0)
                                {
                                    curPosInfo.timeInSamples = lastPositionData.frame;
                                    curPosInfo.timeInSeconds = double(curPosInfo.timeInSamples)/sampleRate;
                                }
                            }
                            
                            if (lastPositionData.bar >= 0 && lastPositionData.beatsPerBar > 0.0f)
                            {
                                curPosInfo.ppqPositionOfLastBarStart = lastPositionData.bar * lastPositionData.beatsPerBar;
                                
                                if (lastPositionData.barBeat >= 0.0f)
                                    curPosInfo.ppqPosition = curPosInfo.ppqPositionOfLastBarStart + lastPositionData.barBeat;
                            }
                            
                            lastPositionData.extraValid = (lastPositionData.beatsPerMinute > 0.0 &&
                                                           lastPositionData.beatUnit > 0 &&
                                                           lastPositionData.beatsPerBar > 0.0f);
                        }
                    }
                }
                {
                    AudioSampleBuffer chans (channels, jmax (filter->getTotalNumInputChannels(), filter->getTotalNumOutputChannels()), sampleCount);
                    filter->processBlock (chans, midiEvents);
                }
            }
        }
        
        // update timePos for next callback
        if (lastPositionData.speed != 0.0)
        {
            if (lastPositionData.speed > 0.0)
            {
                // playing forwards
                lastPositionData.frame += sampleCount;
            }
            else
            {
                // playing backwards
                lastPositionData.frame -= sampleCount;
                
                if (lastPositionData.frame < 0)
                    lastPositionData.frame = 0;
            }
            
            curPosInfo.timeInSamples = lastPositionData.frame;
            curPosInfo.timeInSeconds = double(curPosInfo.timeInSamples)/sampleRate;
            
            if (lastPositionData.extraValid)
            {
                const double beatsPerMinute = lastPositionData.beatsPerMinute * lastPositionData.speed;
                const double framesPerBeat  = 60.0 * sampleRate / beatsPerMinute;
                const double addedBarBeats  = double(sampleCount) / framesPerBeat;
                
                if (lastPositionData.bar >= 0 && lastPositionData.barBeat >= 0.0f)
                {
                    lastPositionData.bar    += std::floor((lastPositionData.barBeat+addedBarBeats)/
                                                          lastPositionData.beatsPerBar);
                    lastPositionData.barBeat = std::fmod(lastPositionData.barBeat+addedBarBeats,
                                                         lastPositionData.beatsPerBar);
                    
                    if (lastPositionData.bar < 0)
                        lastPositionData.bar = 0;
                    
                    curPosInfo.ppqPositionOfLastBarStart = lastPositionData.bar * lastPositionData.beatsPerBar;
                    curPosInfo.ppqPosition = curPosInfo.ppqPositionOfLastBarStart + lastPositionData.barBeat;
                }
                
                curPosInfo.bpm = std::abs(beatsPerMinute);
            }
        }
        
        if (filter->producesMidi() && portMidiOut != nullptr)
        {
            const uint32_t capacity = portMidiOut->atom.size;
            
            portMidiOut->atom.size = sizeof(LV2_Atom_Sequence_Body);
            portMidiOut->atom.type = uridAtomSequence;
            portMidiOut->body.unit = 0;
            portMidiOut->body.pad  = 0;
            
            uint32_t offset = 0;
            for(auto it = midiEvents.cbegin(); it != midiEvents.cend(); ++it)
            {
                if (sizeof(LV2_Atom_Event) + (*it).numBytes > capacity - offset)
                    break;
                
                auto* aev = (LV2_Atom_Event*)((char*)LV2_ATOM_CONTENTS(LV2_Atom_Sequence, portMidiOut) + offset);
                aev->time.frames = (*it).samplePosition;
                aev->body.type   = uridMidiEvent;
                aev->body.size   = (*it).numBytes;
                memcpy(LV2_ATOM_BODY(&aev->body), (*it).data, (*it).numBytes);
                
                auto const size = lv2_atom_pad_size(sizeof(LV2_Atom_Event) + (*it).numBytes);
                offset += size;
                portMidiOut->atom.size += size;
            }
            midiEvents.clear();
            
        }
        else if (! midiEvents.isEmpty())
        {
            midiEvents.clear();
        }
    }
    
    //==============================================================================
    // LV2 extended calls
    
    uint32_t lv2GetOptions (LV2_Options_Option* options)
    {
        // currently unused
        ignoreUnused(options);
        
        return LV2_OPTIONS_SUCCESS;
    }
    
    uint32_t lv2SetOptions (const LV2_Options_Option* options)
    {
        for (int j=0; options[j].key != 0; ++j)
        {
            if (options[j].key == uridMap->map(uridMap->handle, LV2_BUF_SIZE__nominalBlockLength))
            {
                if (options[j].type == uridAtomInt)
                    bufferSize = *(int*)options[j].value;
                else
                    std::cerr << "Host changed nominalBlockLength but with wrong value type" << std::endl;
            }
            else if (options[j].key == uridMap->map(uridMap->handle, LV2_BUF_SIZE__maxBlockLength) && ! usingNominalBlockLength)
            {
                if (options[j].type == uridAtomInt)
                    bufferSize = *(int*)options[j].value;
                else
                    std::cerr << "Host changed maxBlockLength but with wrong value type" << std::endl;
            }
            else if (options[j].key == uridMap->map(uridMap->handle, LV2_CORE__sampleRate))
            {
                if (options[j].type == uridAtomDouble)
                    sampleRate = *(double*)options[j].value;
                else
                    std::cerr << "Host changed sampleRate but with wrong value type" << std::endl;
            }
        }
        
        return LV2_OPTIONS_SUCCESS;
    }
    
    const LV2_Program_Descriptor* lv2GetProgram (uint32_t index)
    {
        jassert (filter != nullptr);
        
        if (progDesc.name != nullptr)
        {
            free((void*)progDesc.name);
            progDesc.name = nullptr;
        }
        
        if ((int)index < filter->getNumPrograms())
        {
            progDesc.bank    = index / 128;
            progDesc.program = index % 128;
            progDesc.name    = strdup(filter->getProgramName(index).toUTF8());
            return &progDesc;
        }
        
        return nullptr;
    }
    
    void lv2SelectProgram (uint32_t bank, uint32_t program)
    {
        jassert (filter != nullptr);
        
        int realProgram = bank * 128 + program;
        
        if (realProgram < filter->getNumPrograms())
        {
            filter->setCurrentProgram(realProgram);
            
            // update input control ports now
            for (int i = 0; i < portControls.size(); ++i)
            {
                float value = getParameterValue(i);
                
                if (portControls[i] != nullptr)
                    *portControls[i] = value;
            }
        }
    }
    
    LV2_State_Status lv2SaveState (LV2_State_Store_Function store, LV2_State_Handle stateHandle)
    {
        jassert (filter != nullptr);
        MemoryBlock chunkMemory;
        filter->getCurrentProgramStateInformation (chunkMemory);
        
        store (stateHandle,
               uridMap->map(uridMap->handle, JUCE_LV2_STATE_BINARY_URI),
               chunkMemory.getData(),
               chunkMemory.getSize(),
               uridMap->map(uridMap->handle, LV2_ATOM__Chunk),
               LV2_STATE_IS_POD|LV2_STATE_IS_PORTABLE);

        return LV2_STATE_SUCCESS;
    }
    
    LV2_State_Status lv2RestoreState (LV2_State_Retrieve_Function retrieve, LV2_State_Handle stateHandle, uint32_t flags)
    {
        jassert (filter != nullptr);
        
        size_t size = 0;
        uint32 type = 0;
        const void* data = retrieve (stateHandle, uridMap->map(uridMap->handle, JUCE_LV2_STATE_BINARY_URI), &size, &type, &flags);
        
        if (data == nullptr || size == 0 || type == 0)
            return LV2_STATE_ERR_UNKNOWN;
        
        if (type == uridMap->map (uridMap->handle, LV2_ATOM__Chunk))
        {
            filter->setCurrentProgramStateInformation (data, static_cast<int>(size));
            
            if (ui != nullptr)
                ui->repaint();
 
            return LV2_STATE_SUCCESS;
        }
        return LV2_STATE_ERR_BAD_TYPE;
    }
    
    //==============================================================================
    // Juce calls
    
    bool getCurrentPosition (AudioPlayHead::CurrentPositionInfo& info)
    {
        info = curPosInfo;
        return true;
    }
    
    //==============================================================================
    JuceLv2UIWrapper* instantiateUI (LV2UI_Write_Function writeFunction, LV2UI_Controller controller, LV2UI_Widget* widget, const LV2_Feature* const* features, bool isExternal)
    {
        const MessageManagerLock mmLock;
        
        if (ui != nullptr)
            ui->resetIfNeeded (writeFunction, controller, widget, features);
        else
            ui = std::make_unique<JuceLv2UIWrapper>(inParameterChangedCallback, filter.get(), writeFunction, controller, widget, features, isExternal);
        
        return ui.get();
    }
    
    ThreadLocalValue<bool> inParameterChangedCallback;
private:
#if JUCE_LINUX
    SharedResourcePointer<SharedMessageThread> msgThread;
#else
    SharedResourcePointer<ScopedJuceInitialiser_GUI> sharedJuceGUI;
#endif
    
    std::unique_ptr<AudioProcessor> filter;
    std::unique_ptr<JuceLv2UIWrapper> ui;
    HeapBlock<float*> channels;
    MidiBuffer midiEvents;
    
    LV2_Atom_Sequence* portEventsIn = nullptr;
    LV2_Atom_Sequence* portMidiOut = nullptr;
    float* portFreewheel = nullptr;
    float* portLatency = nullptr;
    std::vector<float*> portAudioIns;
    std::vector<float*> portAudioOuts;
    std::vector<float*> portControls;
    
    uint32 bufferSize;
    double sampleRate;
    
    AudioPlayHead::CurrentPositionInfo curPosInfo;
    
    struct Lv2PositionData {
        int64_t  bar;
        float    barBeat;
        uint32_t beatUnit;
        float    beatsPerBar;
        float    beatsPerMinute;
        int64_t  frame;
        double   speed;
        bool     extraValid;
        
        Lv2PositionData()
        : bar(-1),
        barBeat(-1.0f),
        beatUnit(0),
        beatsPerBar(0.0f),
        beatsPerMinute(0.0f),
        frame(-1),
        speed(0.0),
        extraValid(false) {}
    };
    Lv2PositionData lastPositionData;
    
    LV2_URID_Map const* uridMap = nullptr;
    LV2_URID uridAtomBlank = 0;
    LV2_URID uridAtomObject = 0;
    LV2_URID uridAtomDouble = 0;
    LV2_URID uridAtomFloat = 0;
    LV2_URID uridAtomInt = 0;
    LV2_URID uridAtomLong = 0;
    LV2_URID uridAtomSequence = 0;
    LV2_URID uridMidiEvent = 0;
    LV2_URID uridTimePos = 0;
    LV2_URID uridTimeBar = 0;
    LV2_URID uridTimeBarBeat = 0;
    LV2_URID uridTimeBeatsPerBar = 0;
    LV2_URID uridTimeBeatsPerMinute = 0;
    LV2_URID uridTimeBeatUnit = 0;
    LV2_URID uridTimeFrame = 0;
    LV2_URID uridTimeSpeed = 0;
    bool usingNominalBlockLength = false;
    
    LV2_Program_Descriptor progDesc;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JuceLv2Wrapper)
};

//==============================================================================
// LV2 descriptor functions

static LV2_Handle juceLV2_Instantiate (const LV2_Descriptor*, double sampleRate, const char*, const LV2_Feature* const* features)
{
    auto createProcessor = []()
    {
        const MessageManagerLock mmLock;
        return std::unique_ptr<AudioProcessor>(createPluginFilterOfType (AudioProcessor::wrapperType_LV2));
    };
    return new JuceLv2Wrapper(createProcessor(), sampleRate, features);
}

static void juceLV2_ConnectPort (LV2_Handle handle, uint32 port, void* dataLocation)
{
    ((JuceLv2Wrapper*)handle)->lv2ConnectPort (port, dataLocation);
}

static void juceLV2_Activate (LV2_Handle handle)
{
    ((JuceLv2Wrapper*)handle)->lv2Activate();
}

static void juceLV2_Run( LV2_Handle handle, uint32 sampleCount)
{
    ((JuceLv2Wrapper*)handle)->lv2Run (sampleCount);
}

static void juceLV2_Deactivate (LV2_Handle handle)
{
    ((JuceLv2Wrapper*)handle)->lv2Deactivate();
}

static void juceLV2_Cleanup (LV2_Handle handle)
{
    delete ((JuceLv2Wrapper*)handle);
}

//==============================================================================
// LV2 extended functions

static uint32_t juceLV2_getOptions (LV2_Handle handle, LV2_Options_Option* options)
{
    return ((JuceLv2Wrapper*)handle)->lv2GetOptions(options);
}

static uint32_t juceLV2_setOptions (LV2_Handle handle, const LV2_Options_Option* options)
{
    return ((JuceLv2Wrapper*)handle)->lv2SetOptions(options);
}

static const LV2_Program_Descriptor* juceLV2_getProgram (LV2_Handle handle, uint32_t index)
{
    return ((JuceLv2Wrapper*)handle)->lv2GetProgram(index);
}

static void juceLV2_selectProgram (LV2_Handle handle, uint32_t bank, uint32_t program)
{
    ((JuceLv2Wrapper*)handle)->lv2SelectProgram(bank, program);
}

static LV2_State_Status juceLV2_SaveState (LV2_Handle handle, LV2_State_Store_Function store, LV2_State_Handle stateHandle, uint32_t, const LV2_Feature* const*)
{
    return ((JuceLv2Wrapper*)handle)->lv2SaveState(store, stateHandle);
}

static LV2_State_Status juceLV2_RestoreState (LV2_Handle handle, LV2_State_Retrieve_Function retrieve, LV2_State_Handle stateHandle, uint32_t flags, const LV2_Feature* const*)
{
    return ((JuceLv2Wrapper*)handle)->lv2RestoreState(retrieve, stateHandle, flags);
}

static const void* juceLV2_ExtensionData (const char* uri)
{
    static const LV2_Options_Interface options = { juceLV2_getOptions, juceLV2_setOptions };
    static const LV2_Programs_Interface programs = { juceLV2_getProgram, juceLV2_selectProgram };
    static const LV2_State_Interface state = { juceLV2_SaveState, juceLV2_RestoreState };
    
    if (strcmp(uri, LV2_OPTIONS__interface) == 0)
        return &options;
    if (strcmp(uri, LV2_PROGRAMS__Interface) == 0)
        return &programs;
    if (strcmp(uri, LV2_STATE__interface) == 0)
        return &state;
    
    return nullptr;
}

//==============================================================================
// LV2 UI descriptor functions

static LV2UI_Handle juceLV2UI_Instantiate (LV2UI_Write_Function writeFunction, LV2UI_Controller controller, LV2UI_Widget* widget, const LV2_Feature* const* features, bool isExternal)
{
    for (int i = 0; features[i] != nullptr; ++i)
    {
        if (strcmp(features[i]->URI, LV2_INSTANCE_ACCESS_URI) == 0 && features[i]->data != nullptr)
        {
            JuceLv2Wrapper* wrapper = static_cast<JuceLv2Wrapper*>(features[i]->data);
            return wrapper->instantiateUI(writeFunction, controller, widget, features, isExternal);
        }
    }
    
    return nullptr;
}

static LV2UI_Handle juceLV2UI_InstantiateExternal (const LV2UI_Descriptor*, const char*, const char*, LV2UI_Write_Function writeFunction,
                                                   LV2UI_Controller controller, LV2UI_Widget* widget, const LV2_Feature* const* features)
{
    return juceLV2UI_Instantiate(writeFunction, controller, widget, features, true);
}

static LV2UI_Handle juceLV2UI_InstantiateParent (const LV2UI_Descriptor*, const char*, const char*, LV2UI_Write_Function writeFunction,
                                                 LV2UI_Controller controller, LV2UI_Widget* widget, const LV2_Feature* const* features)
{
    return juceLV2UI_Instantiate(writeFunction, controller, widget, features, false);
}

static void juceLV2UI_Cleanup (LV2UI_Handle handle)
{
    ((JuceLv2UIWrapper*)handle)->lv2Cleanup();
}


#if JUCE_WINDOWS
#define JUCE_EXPORTED_FUNCTION extern "C" __declspec (dllexport)
#else
#define JUCE_EXPORTED_FUNCTION extern "C" __attribute__ ((visibility("default")))
#endif

//==============================================================================
// startup code..

JUCE_EXPORTED_FUNCTION const LV2_Descriptor* lv2_descriptor (uint32 index);
JUCE_EXPORTED_FUNCTION const LV2_Descriptor* lv2_descriptor (uint32 index)
{
    if(index == 0)
    {
        static const String pluginUri(JucePlugin_LV2URI);
        static const LV2_Descriptor JuceLv2Plugin =
        {
            pluginUri.toRawUTF8(),
            juceLV2_Instantiate,
            juceLV2_ConnectPort,
            juceLV2_Activate,
            juceLV2_Run,
            juceLV2_Deactivate,
            juceLV2_Cleanup,
            juceLV2_ExtensionData
        };
        return &JuceLv2Plugin;
    }
    return nullptr;
}

JUCE_EXPORTED_FUNCTION const LV2UI_Descriptor* lv2ui_descriptor (uint32 index);
JUCE_EXPORTED_FUNCTION const LV2UI_Descriptor* lv2ui_descriptor (uint32 index)
{
    switch (index)
    {
        case 0:
        {
            static const String pluginUiUri(juce::String(JucePlugin_LV2URI) + juce::String("#ExternalUI"));
            static const LV2UI_Descriptor JuceLv2UI_External =
            {
                pluginUiUri.toRawUTF8(),
                juceLV2UI_InstantiateExternal,
                juceLV2UI_Cleanup,
                nullptr,
                nullptr
            };
            return &JuceLv2UI_External;
        }
            
        case 1:
        {
            static const String pluginUiUri(juce::String(JucePlugin_LV2URI) + juce::String("#ParentUI"));
            static const LV2UI_Descriptor JuceLv2UI_Parent =
            {
                pluginUiUri.toRawUTF8(),
                juceLV2UI_InstantiateParent,
                juceLV2UI_Cleanup,
                nullptr,
                nullptr
            };
            return &JuceLv2UI_Parent;
        }
        default:
            return nullptr;
    }
}

#endif
