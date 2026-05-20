/**
 * MidiLearnMap.h
 * Data model and UI panel for the MIDI Learn feature.
 *
 * MidiLearnMap  — stores CC→OSC-address bindings, serialises to/from juce::var.
 * MidiLearnPanel — TabbedComponent-ready scrollable table with [Learn]/[X] per row.
 *
 * Thread safety: bind/resolve are protected by a CriticalSection held by the
 * caller (MainComponent::midiLearnLock). UI callbacks all run on the message thread.
 */
#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <unordered_map>
#include <functional>

// ── Data model ────────────────────────────────────────────────────────────────

enum class LearnMode { absolute, relative };

struct MidiLearnBinding
{
    int       cc           = -1;              // -1 = unassigned
    int       channel      =  0;              // 0 = any channel, 1–16 = specific channel
    LearnMode mode         = LearnMode::absolute;
    float     currentValue = 0.5f;            // accumulated position for relative mode
};

class MidiLearnMap
{
public:
    std::unordered_map<juce::String, MidiLearnBinding> bindings;

    void assign (const juce::String& address, int channel, int cc)
    {
        bindings[address] = { cc, channel };
    }

    void clear (const juce::String& address)
    {
        bindings.erase (address);
    }

    /** Returns the first OSC address bound to {channel, cc}, or an empty string. */
    juce::String resolve (int channel, int cc) const
    {
        for (auto& [addr, b] : bindings)
            if (b.cc == cc && (b.channel == 0 || b.channel == channel))
                return addr;
        return {};
    }

    /** Serialise to a juce::var (DynamicObject tree) for JSON storage. */
    juce::var toVar() const
    {
        auto root = std::make_unique<juce::DynamicObject>();
        for (auto& [addr, b] : bindings)
        {
            auto entry = std::make_unique<juce::DynamicObject>();
            entry->setProperty ("cc",      b.cc);
            entry->setProperty ("channel", b.channel);
            entry->setProperty ("mode",    b.mode == LearnMode::relative ? "relative" : "absolute");
            root->setProperty (addr, juce::var (entry.release()));
        }
        return juce::var (root.release());
    }

    void fromVar (const juce::var& v)
    {
        bindings.clear();
        auto* root = v.getDynamicObject();
        if (root == nullptr) return;
        for (auto& prop : root->getProperties())
        {
            auto* entry = prop.value.getDynamicObject();
            if (entry == nullptr) continue;
            MidiLearnBinding b;
            b.cc      = static_cast<int> (entry->getProperty ("cc"));
            b.channel = static_cast<int> (entry->getProperty ("channel"));
            b.mode    = entry->getProperty ("mode").toString() == "relative"
                            ? LearnMode::relative : LearnMode::absolute;
            bindings[prop.name.toString()] = b;
        }
    }
};

// ── MIDI Learn panel ─────────────────────────────────────────────────────────

class MidiLearnPanel : public juce::Component,
                       private juce::TableListBoxModel
{
public:
    // Callbacks set by MainComponent after construction
    std::function<void()>    onQueryRequested;
    std::function<void(int)> onLearnRequested;
    std::function<void(int)> onClearRequested;
    std::function<void(int)> onModeToggleRequested;

    MidiLearnPanel (juce::StringArray& params, MidiLearnMap& map)
        : paramAddresses (params), learnMap (map)
    {
        queryButton.setButtonText ("Query SurgeXT Parameters");
        queryButton.setColour (juce::TextButton::buttonColourId,
                               juce::Colour::fromRGB (40, 90, 140));
        queryButton.onClick = [this] { if (onQueryRequested) onQueryRequested(); };
        addAndMakeVisible (queryButton);

        learnStatusLabel.setColour (juce::Label::textColourId, juce::Colours::orangered);
        learnStatusLabel.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (learnStatusLabel);

        table.getHeader().addColumn ("OSC Address", 1, 340, 200, 2000,
                                     juce::TableHeaderComponent::notSortable);
        table.getHeader().addColumn ("CC",          2,  52,  40,   80,
                                     juce::TableHeaderComponent::notSortable);
        table.getHeader().addColumn ("Ch",          3,  40,  30,   60,
                                     juce::TableHeaderComponent::notSortable);
        table.getHeader().addColumn ("Mode",        4,  40,  40,   60,
                                     juce::TableHeaderComponent::notSortable);
        table.getHeader().addColumn ("",            5, 110, 110,  110,
                                     juce::TableHeaderComponent::notSortable);
        table.setModel (this);
        table.setRowHeight (22);
        table.setColour (juce::ListBox::backgroundColourId,
                         juce::Colour::fromRGB (20, 30, 38));
        addAndMakeVisible (table);
    }

    /** Call on the message thread when learn mode starts (row >= 0) or stops (row = -1). */
    void setLearningRow (int row)
    {
        const int prev = learningRow;
        learningRow = row;
        learnStatusLabel.setText (row >= 0 ? "Move a CC on your controller..."
                                           : juce::String{},
                                  juce::dontSendNotification);
        if (prev >= 0) table.repaintRow (prev);
        if (row >= 0)  table.repaintRow (row);
    }

    /** Call after the param list or bindings change to redraw the table. */
    void refresh()
    {
        table.updateContent();
        table.repaint();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (6, 4);
        auto topBar = area.removeFromTop (28);
        queryButton.setBounds (topBar.removeFromLeft (220));
        topBar.removeFromLeft (10);
        learnStatusLabel.setBounds (topBar);
        area.removeFromTop (4);
        table.setBounds (area);
    }

private:
    // ── TableListBoxModel ────────────────────────────────────────────────────
    int getNumRows() override
    {
        return paramAddresses.size();
    }

    void paintRowBackground (juce::Graphics& g, int row, int, int, bool sel) override
    {
        if (row == learningRow)
            g.fillAll (juce::Colour::fromRGB (90, 34, 10));
        else if (sel)
            g.fillAll (juce::Colour::fromRGB (40, 55, 70));
        else
            g.fillAll (row % 2 == 0 ? juce::Colour::fromRGB (20, 30, 38)
                                    : juce::Colour::fromRGB (25, 37, 47));
    }

    void paintCell (juce::Graphics& g, int row, int columnId,
                    int width, int height, bool) override
    {
        if (row >= paramAddresses.size() || columnId == 4 || columnId == 5) return;

        const auto& addr = paramAddresses.getReference (row);
        const auto it = learnMap.bindings.find (addr);
        const bool bound = it != learnMap.bindings.end() && it->second.cc >= 0;

        g.setFont (juce::FontOptions (12.0f));
        juce::String text;

        switch (columnId)
        {
            case 1:
                text = addr;
                g.setColour (bound ? juce::Colours::white : juce::Colour::fromRGB (140, 155, 160));
                break;
            case 2:
                text = bound ? juce::String (it->second.cc) : "-";
                g.setColour (bound ? juce::Colours::limegreen : juce::Colours::dimgrey);
                break;
            case 3:
                text = bound ? (it->second.channel == 0 ? "*"
                                                        : juce::String (it->second.channel)) : "-";
                g.setColour (bound ? juce::Colours::limegreen : juce::Colours::dimgrey);
                break;
            default: return;
        }
        g.drawText (text, 4, 0, width - 4, height, juce::Justification::centredLeft);
    }

    juce::Component* refreshComponentForCell (int row, int columnId, bool,
                                              juce::Component* existing) override
    {
        const bool inRange = row < paramAddresses.size();

        // Column 4: A/R mode toggle (only shown when bound)
        if (columnId == 4)
        {
            if (! inRange) { delete existing; return nullptr; }
            const auto& addr = paramAddresses.getReference (row);
            const bool bound = learnMap.bindings.count (addr) > 0
                               && learnMap.bindings.at (addr).cc >= 0;
            if (! bound) { delete existing; return nullptr; }

            auto* btn = dynamic_cast<juce::TextButton*> (existing);
            if (btn == nullptr) btn = new juce::TextButton();
            const bool isRel = learnMap.bindings.at (addr).mode == LearnMode::relative;
            btn->setButtonText (isRel ? "Rel" : "Abs");
            btn->setColour (juce::TextButton::buttonColourId,
                            isRel ? juce::Colour::fromRGB (100, 60, 10)
                                  : juce::Colour::fromRGB (20, 70, 40));
            btn->onClick = [this, row] { if (onModeToggleRequested) onModeToggleRequested (row); };
            return btn;
        }

        // Column 5: Learn / Clear buttons
        if (columnId == 5)
        {
            auto* cell = dynamic_cast<LearnClearCell*> (existing);
            if (cell == nullptr) cell = new LearnClearCell();
            const bool bound = inRange
                               && learnMap.bindings.count (paramAddresses.getReference (row)) > 0
                               && learnMap.bindings.at (paramAddresses.getReference (row)).cc >= 0;
            cell->onLearn = [this, row] { if (onLearnRequested) onLearnRequested (row); };
            cell->onClear = [this, row] { if (onClearRequested) onClearRequested (row); };
            cell->update (bound, row == learningRow);
            return cell;
        }

        delete existing;
        return nullptr;
    }

    // ── Inline [Learn] / [X] button pair ─────────────────────────────────────
    struct LearnClearCell : public juce::Component
    {
        juce::TextButton learnBtn, clearBtn { "X" };
        std::function<void()> onLearn, onClear;

        LearnClearCell()
        {
            learnBtn.setColour (juce::TextButton::buttonColourId,
                                juce::Colour::fromRGB (40, 70, 100));
            clearBtn.setColour (juce::TextButton::buttonColourId,
                                juce::Colour::fromRGB (110, 30, 30));
            clearBtn.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
            learnBtn.onClick = [this] { if (onLearn) onLearn(); };
            clearBtn.onClick = [this] { if (onClear) onClear(); };
            addAndMakeVisible (learnBtn);
            addAndMakeVisible (clearBtn);
        }

        void update (bool isBound, bool isLearning)
        {
            learnBtn.setButtonText (isLearning ? "..." : "Learn");
            learnBtn.setColour (juce::TextButton::buttonColourId,
                                isLearning ? juce::Colours::orangered
                                           : juce::Colour::fromRGB (40, 70, 100));
            clearBtn.setEnabled (isBound);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (2, 3);
            clearBtn.setBounds (r.removeFromRight (22));
            r.removeFromRight (2);
            learnBtn.setBounds (r);
        }
    };

    // ── Members ───────────────────────────────────────────────────────────────
    juce::StringArray&  paramAddresses;
    MidiLearnMap&       learnMap;
    juce::TableListBox  table { {}, this };
    juce::TextButton    queryButton;
    juce::Label         learnStatusLabel;
    int                 learningRow = -1;
};
