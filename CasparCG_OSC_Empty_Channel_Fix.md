# CasparCG OSC Monitoring Gap: Empty Channel Detection

## Problem Statement

**Current Issue**: When a producer is destroyed or removed from a stage using `CLEAR` commands, no OSC monitoring signal is sent to indicate the stage has gone empty. The layer simply disappears from OSC output entirely.

## Root Cause Analysis

### Current Clear Implementation
In `src/core/producer/stage.cpp`:
```cpp
std::future<void> clear(int index)
{
    return executor_.begin_invoke([=] { layers_.erase(index); });
}
```

### State Generation Process
```cpp
monitor::state state;
for (auto& p : layers_) {
    state["layer"][p.first] = p.second.state();
}
```

**The Problem**: When `layers_.erase(index)` is called, the layer is completely removed from the map. During state generation, only existing layers are iterated, so cleared layers produce no OSC output at all.

## Potential Solutions

### Solution 1: Send Final Empty State Before Removal

Modify the clear methods to send a final "empty" state before removing the layer:

```cpp
std::future<void> clear(int index)
{
    return executor_.begin_invoke([=] {
        // Send final empty state before removal
        auto it = layers_.find(index);
        if (it != layers_.end()) {
            // Force the layer to show as empty
            it->second.stop(); // This sets foreground to frame_producer::empty()
            
            // Trigger immediate state update for this layer
            monitor::state empty_state;
            empty_state["layer"][index] = it->second.state();
            // Send the empty state via OSC here
            
            // Then remove the layer
            layers_.erase(index);
        }
    });
}
```

### Solution 2: Track Recently Cleared Layers

Maintain a list of recently cleared layers and include them in state generation:

```cpp
struct stage::impl {
    // ... existing members ...
    std::map<int, std::chrono::steady_clock::time_point> recently_cleared_;
    static constexpr auto CLEAR_NOTIFICATION_DURATION = std::chrono::seconds(1);
    
    std::future<void> clear(int index) {
        return executor_.begin_invoke([=] {
            recently_cleared_[index] = std::chrono::steady_clock::now();
            layers_.erase(index);
        });
    }
    
    // In state generation:
    monitor::state state;
    
    // Include existing layers
    for (auto& p : layers_) {
        state["layer"][p.first] = p.second.state();
    }
    
    // Include recently cleared layers as empty
    auto now = std::chrono::steady_clock::now();
    for (auto it = recently_cleared_.begin(); it != recently_cleared_.end();) {
        if (now - it->second < CLEAR_NOTIFICATION_DURATION) {
            monitor::state empty_layer_state;
            empty_layer_state["foreground"]["producer"] = "empty";
            empty_layer_state["background"]["producer"] = "empty";
            state["layer"][it->first] = empty_layer_state;
            ++it;
        } else {
            it = recently_cleared_.erase(it);
        }
    }
};
```

### Solution 3: Global Layer Registry

Maintain a registry of all layer indices that have ever been used, and explicitly mark them as empty:

```cpp
struct stage::impl {
    std::set<int> known_layers_;
    
    layer& get_layer(int index) {
        known_layers_.insert(index);
        auto it = layers_.find(index);
        if (it == std::end(layers_)) {
            it = layers_.emplace(index, layer(video_format_desc())).first;
        }
        return it->second;
    }
    
    // In state generation:
    monitor::state state;
    for (int layer_index : known_layers_) {
        auto it = layers_.find(layer_index);
        if (it != layers_.end()) {
            state["layer"][layer_index] = it->second.state();
        } else {
            // Explicitly mark as empty
            monitor::state empty_state;
            empty_state["foreground"]["producer"] = "empty";
            empty_state["background"]["producer"] = "empty";
            state["layer"][layer_index] = empty_state;
        }
    }
};
```

### Solution 4: Configuration-Based Layer Monitoring

Allow configuration of which layers to monitor, always reporting their state:

```cpp
// In configuration
std::set<int> monitored_layers_ = {1, 2, 3, 4, 5, 10, 20}; // configurable

// In state generation:
monitor::state state;
for (int layer_index : monitored_layers_) {
    auto it = layers_.find(layer_index);
    if (it != layers_.end()) {
        state["layer"][layer_index] = it->second.state();
    } else {
        monitor::state empty_state;
        empty_state["foreground"]["producer"] = "empty";
        empty_state["background"]["producer"] = "empty";
        state["layer"][layer_index] = empty_state;
    }
}
```

## Recommended Implementation

**Solution 2 (Track Recently Cleared Layers)** appears most balanced because:

1. **Minimal Performance Impact**: Only tracks cleared layers for a short duration
2. **Backward Compatible**: Doesn't change existing behavior for unused layers
3. **Reliable Notification**: Guarantees OSC clients receive empty notifications
4. **Automatic Cleanup**: Recently cleared layers expire automatically

## Implementation Steps

1. **Modify stage::impl structure** to include recently cleared tracking
2. **Update clear() methods** to record clear timestamps
3. **Modify state generation** to include recently cleared layers as empty
4. **Add configuration option** for notification duration
5. **Test with OSC monitoring tools** to verify proper empty detection

## Alternative: External Solution

For immediate implementation without modifying CasparCG source:

1. **Monitor AMCP commands**: Parse incoming `CLEAR` commands
2. **Track layer states**: Maintain external state of what's loaded where
3. **Generate synthetic OSC**: Send artificial "empty" messages when detecting clears
4. **Proxy OSC output**: Intercept and augment existing OSC stream

This could be implemented as a middleware service that sits between AMCP clients and CasparCG server.