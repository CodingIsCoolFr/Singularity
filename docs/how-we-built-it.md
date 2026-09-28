# How Singularity was built

A Discord client written from scratch in C++20 and Qt 6, for Windows. Not a patch on Discord's app, and not Electron with their page inside it. One process. The plugins are compiled into the binary, so there is no folder on disk for anything else to swap out.

The source is private. The installers live in a [separate repository](https://github.com/CodingIsCoolFr/singularity-updates), because the updater wants a setup program, not this tree. This note is public there, and on the site.

Discord does not permit third party clients on a normal user account. Running this can get the account banned. That risk is taken on purpose.

## Why it is not a browser

Electron would have been the short way. It is also six processes and about a gigabyte of memory to sit idle, which is what Discord's own app costs on the same machine. Singularity is one process and, sitting idle, 178 MB.

The window is Qt widgets. Messages are drawn by Qt, not by a page. The one web view is the captcha window: Discord's check is a page they serve, so that one window is WebView2 and nothing else is.

## The shape

Four pieces, and they do not share a thread when the work is heavy.

| | |
| --- | --- |
| `GatewayClient` | The live socket. Events arrive here. |
| `RestClient` | Anything that is a request: send, edit, a member patch, an upload. |
| `MessageStore` | What the window draws from. The window does not parse JSON on its own. |
| `VoiceConnection` | One for the call, one for a stream being watched, one for a share of our own. |

The window thread paints. Capture, encoding, and the voice sockets do not run on it. A 4K frame is thirty three megabytes. Passing one between threads sixty times a second costs more than the encoding, so capture and encode share one thread, and that thread is not the one drawing the window.

A hang watcher times the window thread. If it stalls, the log names what it was doing. A stall with no name above it is noise, so the next time a path can block, it gets a line first.

Plugins are C++ classes linked in. A hook that returns false cancels the thing the client was about to do. The first no wins. There is no plugin folder.

## Talking to Discord

The gateway is the live stream of events. REST is everything you ask for. Voice is a different socket, on a machine the gateway names when you join. A shared screen is a third socket. The call underneath has to stay up, or you have left the channel.

The voice address arrives as `host:port`. Dropping the port reaches a different machine. That machine answers, then closes with `4006`. It looks like progress. It is not.

Since 1 March 2026 Discord accepts only end to end encrypted calls. That is [libdave](https://github.com/discord/libdave), MLS, on top of the transport encryption. The recognised-user list has to include your own id, because you are in the group too. Leaving yourself out is refused with "Welcome message lists unrecognized user ID", and only when the channel already has somebody in it. An empty channel seems fine, which is how that one hid.

## A call, and the ways it goes silent

Every one of these presents as plain silence.

**The extension body is encrypted.** In the `rtpsize` modes, only the fixed RTP header, the CSRCs, and the four byte extension preamble are in the clear. The extension elements ride with the sound and are sealed with it. Counting them as header makes every packet fail to open, and Discord sets that bit on nearly all of them.

**Qt does not mix.** Two streams written to one `QAudioSink` are queued, not blended. That is heard as chopped, rushed speech. Each person gets a jitter buffer that measures their line and picks its own cushion. A 20 ms tick sums the frames that are due into one, and Opus fills in a packet that never arrived.

**A phone only plays sound that is marked.** Discord's own client puts two marks on every sound packet, in the one-byte RTP extension form (`0xBEDE`). The preamble stays in the clear. The marks are sealed with the sound, the same as a picture's marks.

| ID | What it is |
| --- | --- |
| 1 | How loud, and whether this packet has sound in it. The high bit is that. The low 7 bits are how quiet, in −dB, 0 being the loudest. Silence is 127 with the high bit clear. A phone's server only forwards a packet that says it has sound. |
| 9 | What kind of sound. `0x04` is the stream's sound. `0x02` is a voice. The websocket speaking flag is not what a phone reads. A packet with this missing is treated as a voice, and a phone watching the stream ignores it. |

The speaking flags Discord documents are 1 for a voice and 2 for a stream's sound. The byte in the packet is that value shifted: `((flags & 3) << 1)`. A stream is 2, so the byte is 4. Opus is offered with encode and decode both true. Without encode, the server does not build a path for what you send, and a phone never receives it.

The picture already carried its own marks, which is why a phone could see a share and hear nothing. The sound was leaving unmarked.

The log prints a tally every five seconds: sent, played, and a named reason for every frame thrown away. That line exists because reading it found in one run what guessing could not.

## Pictures

A camera rides the voice connection. Sound and pictures share the socket and are told apart by the payload type. Discord sends none of it unless you ask. Identify carries `video: true`, meaning this connection understands pictures. It defaults to false, and false is a promise never to be sent one.

The layers you offer are the layers you will send. We used to offer a half-size layer, rid `"50"`, and never send it. The server believed it existed and handed it to anyone who asked for a small picture — a camera in a grid, a phone — and they got nothing. A viewer who asked for full size saw it fine. Offer rid `"100"` only, and send that.

A shared screen does not ride the call. It has its own server, its own websocket, and its own packets. Asking to watch one is opcode 20 on the gateway, with a key `guild:<guild>:<channel>:<user>`. Opening your own is opcode 18. Closing it is 19. The answer comes back as two events, the same shape as joining voice.

A frame is too big for one packet. H.264 is chopped on the way out: a whole piece, several bundled, or one piece split across many. The last packet of a picture is flagged. That is the only way the far end knows the picture is finished. Each sender needs their own decoder, because a decoder holds the earlier frames that later ones are described as changes from.

Loss is the part that decides whether it works. A decoder that misses a frame cannot draw until a keyframe arrives, and the sender will not send one unless asked. A loss report goes back when a frame cannot be rebuilt. Without that the tile stays black, and the log cannot say why. The log counts packets, finished pictures, and frames spent waiting on a keyframe, per sender.

The small tiles used to ask for a 180p copy. That copy draws as stripes. Clicking the tile asked for the full picture, which is why it looked fine once it was large. Both sizes now ask for the full layer.

## Sharing a screen

Share screen in the call panel. It asks which screen, then goes live on a connection of its own.

**Captured through Desktop Duplication.** That is the API the Windows compositor already feeds. A Qt screenshot copies the desktop through the CPU every frame. `BitBlt` misses hardware overlays, so video players and many games come out black. Duplication also reports when nothing moved, so a still screen costs nothing.

**Encoded by whatever this machine has.** NVIDIA, AMD, Intel, or Windows' own, in that order, then OpenH264. Hardware first is not about frame rate. Software 1080p costs a core, on the same machine already running whatever is being shared. The FFmpeg here is the LGPL build, so there is no x264. That is GPL, and linking it would put the GPL on all of this.

**Shaped for a conversation.** No B-frames, no look-ahead, one reference. A B-frame is described partly by a picture that has not been sent yet, so the encoder holds frames back. Free in a file. Delay in a call.

**Keyframes on request.** A viewer who joins mid-stream has only been sent descriptions of changes from pictures they never saw. They say so over RTCP and get a fresh one. Several viewers arriving together are answered once.

**The sound is the program's, not the speakers'.** Windows process loopback asks for the sound of one program and the processes it started, or for everything except one program. The second is how a whole-screen share can carry the game without also carrying the call back to the people who are already hearing it. It needs Windows 10 build 20348 or later. Older than that, the picture still goes out and the sound does not, and the log says so.

That sound is 48 kHz, stereo, 16 bit, which is what the Opus encoder on the stream connection takes. It is encoded as audio, not as speech — speech mode would thin music and games out. It leaves on the stream connection, marked as the stream's sound, which is the mark a phone is waiting for.

## The hole

The background is an OpenGL widget, `AuroraWidget`, drawn by the graphics card. A picture, including a GIF, is the other path, drawn with `QPainter`, and that path stays. The two are not mixed into one renderer.

Creating or deleting that OpenGL widget used to freeze the program for the better part of a minute. Qt, by default, turns every widget beside an OpenGL widget into its own window the moment the hole is created, and tears those windows down when the hole is removed. Windows re-reads the display for each one. That is the freeze. A see-through panel over that surface also painted black, which is why the hole used to sit behind solid panels and could not be seen.

`Qt::AA_DontCreateNativeWidgetSiblings` is set before `QApplication` exists. With it, the hole is another layer and the panels stay clear, the same panels a picture shows through. Do not set `WA_TranslucentBackground` on those panels. Do not make them solid again. The glass flag stays on. Flipping it restyles every widget, which is its own stall.

One seed colour retints the whole application at runtime: surfaces, accents, the stylesheet, and the hole's disk. The status dots are the one thing the seed never touches. Green, yellow, and red have to keep meaning whatever else changes.

## What a message can click

In the chat view, only an `<a href>` is clickable. A styled span is not. Reactions, selects, and the Wave button are links with their own schemes, not buttons drawn in HTML. A mention in the composer is a chip. On the way out it becomes Discord's `<@id>`, not the name that was on screen.

A file waiting to be sent has its own Remove. The Cancel on a reply is a different control, and it is hidden unless a reply or an edit is open. An edit has Stop on the same row as the text, and Escape does the same. Leaving an edit clears the box. Leaving the words there would send that message again, as a new one, the next time Enter is pressed.

## What ships

Two repositories, and they have to agree.

| | |
| --- | --- |
| Source | Private. This note is the public write-up. |
| [singularity-updates](https://github.com/CodingIsCoolFr/singularity-updates) | The installers. This is what the running program reads. |

The updater wants an installer, not the source tree. Publishing to one and not the other is how a releases page once said Latest about a build that was not. The channel is published first, because it is the one the program reads. The site's download button is that same latest installer. It does not have a version of its own.

The version is one number in three files: `CMakeLists.txt`, `src/main.cpp`, and the installer script. They are checked against each other before anything is tagged. A tag that already exists is refused. A dirty tree is refused. The tag points at what shipped.

The install is per user and never asks for administrator. There is no service and no driver. Closing the program for an update is not the same as killing it: Qt writes settings on the way out.

## What is not in here

Search. Threads and forum channels. Avatar decorations, which are fetched and not drawn. Sending your own camera — sharing a screen is built, and a webcam is the same pipeline pointed at a different source, and it is not built. A single window rather than a whole screen.

Windows only.
