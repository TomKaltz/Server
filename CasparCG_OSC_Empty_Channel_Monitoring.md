# CasparCG OSC Monitoring for Empty/Cleared Channels

## Summary

**Yes, CasparCG already supports OSC monitoring signals when channels go empty/cleared.** The OSC monitoring system provides real-time state information about all layers, including whether they contain content or are empty.

## Current OSC Monitoring Capabilities

### Layer State Monitoring

CasparCG's OSC implementation automatically sends state updates for each layer that include:

- **Producer name**: The name of the currently loaded producer
- **Foreground state**: Information about the active content on the layer
- **Background state**: Information about content loaded but not yet playing
- **Playback status**: Whether the layer is paused, playing, etc.

### Empty Channel Detection

When a channel/layer is empty or cleared, the OSC monitoring will report:

```
/channel/[channel]/stage/layer/[layer]/foreground/producer = "empty"
```

The key indicator is that the producer name becomes `"empty"` when:
- A layer is cleared using the `CLEAR` command
- A layer has no content loaded
- Content has finished playing and nothing follows

## Implementation Details

### OSC Client Configuration

CasparCG needs to be configured to send OSC updates to your monitoring application. In your `casparcg.config` file:

```xml
<osc>
  <predefined-clients>
    <predefined-client>
      <address>127.0.0.1</address>
      <port>5253</port>
    </predefined-client>
  </predefined-clients>
</osc>
```

### Monitoring Empty States

To detect when a channel goes empty, monitor the OSC path:
```
/channel/[channel_number]/stage/layer/[layer_number]/foreground/producer
```

When this value equals `"empty"`, the layer has no active content.

### State Transitions

The monitoring system will send updates when:
1. **Content is loaded**: Producer name changes from `"empty"` to the actual producer name
2. **Content is cleared**: Producer name changes from actual producer name to `"empty"`
3. **Content finishes**: Producer name may change to `"empty"` (depending on playlist configuration)

## Code References

The implementation can be found in:
- **OSC Client**: `src/protocol/osc/client.cpp` - Handles OSC message sending
- **Layer State**: `src/core/producer/layer.cpp` - Lines 131-142 show state composition
- **Stage Monitoring**: `src/core/producer/stage.cpp` - Lines 218-221 aggregate layer states
- **Channel Integration**: `src/core/video_channel.cpp` - Lines 165-179 send state updates

## Example Monitoring Applications

Several community projects demonstrate OSC monitoring:
- [CasparCG OSC Monitor](https://github.com/duncanbarnes/Caspar-CG-OSC-Monitor) - Basic OSC monitoring tool
- [OSC to WebSockets Monitor](https://github.com/hreinnbeck/casparcg-osc2websockets-monitor) - Web-based monitoring
- [BITC Overlay](https://github.com/GuildTV/ccg-bitc) - Uses OSC for timecode overlay

## Practical Usage

### Simple Empty Detection Script (Node.js)

```javascript
const osc = require('node-osc');

const server = new osc.Server(5253, '127.0.0.1');

server.on('message', (msg) => {
  const path = msg[0];
  const value = msg[1];
  
  // Check for empty channel
  if (path.includes('/foreground/producer') && value === 'empty') {
    const channelMatch = path.match(/\/channel\/(\d+)\/stage\/layer\/(\d+)/);
    if (channelMatch) {
      const channel = channelMatch[1];
      const layer = channelMatch[2];
      console.log(`Channel ${channel} Layer ${layer} is now EMPTY`);
      
      // Your custom logic here
      handleEmptyChannel(channel, layer);
    }
  }
});

function handleEmptyChannel(channel, layer) {
  // Implement your custom response to empty channels
  // e.g., trigger graphics, send notifications, etc.
}
```

## Conclusion

CasparCG's built-in OSC monitoring system already provides comprehensive real-time state information about all channels and layers, including detection of empty/cleared states. No modifications to the CasparCG source code are required - you simply need to:

1. Configure OSC clients in your CasparCG configuration
2. Set up a monitoring application to receive OSC messages
3. Watch for producer names changing to "empty"

This system has been in production use since CasparCG's early versions and is proven reliable for broadcast environments.