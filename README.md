<div align="center">

<img src="brand/banner.png" alt="Singularity" width="100%">

<br>

![C++20](https://img.shields.io/badge/C%2B%2B-20-cdd6e6?style=flat-square&labelColor=07090e)
![Qt 6.10](https://img.shields.io/badge/Qt-6.10-cdd6e6?style=flat-square&labelColor=07090e)
![Windows](https://img.shields.io/badge/platform-Windows-8892a6?style=flat-square&labelColor=07090e)
![Voice](https://img.shields.io/badge/voice-end--to--end%20encrypted-a9c6e0?style=flat-square&labelColor=07090e)
![Plugins](https://img.shields.io/badge/plugins-compiled%20in-8892a6?style=flat-square&labelColor=07090e)

</div>

---

A Discord client written from scratch in C++ with Qt 6. No Electron, no web
view, no plugin folder. The plugins are part of the binary, so there is nothing
on disk for anything else to swap out.

Calls work: you can talk, you can hear, and the audio is end to end encrypted
the way Discord now requires. Cameras and shared screens are received and
decoded as well, and the grid of faces above the conversation is where they
appear.

## Warning, read this first

Discord does not permit third party clients on a normal user account. Running
this can get the account banned. That risk is accepted here on purpose.

What Singularity does with your credentials:

| | |
| --- | --- |
| Your password | Sent to `discord.com` and nowhere else. Never written to disk. Never logged. |
| Your session token | Sealed with your Windows account key, so no other account on the machine can read it. The log records its length, never its value. |
| Captchas | Not answered, ever. If Discord demands one, the sign-in window says so and offers token entry instead. |
| Images | Only fetched from Discord's own hosts, so a stranger's message cannot make your client call an address of their choosing. |

## What it does

<table>
<tr><td width="33%" valign="top">

**Talking**

Voice channels you can actually join, with sound both ways, encrypted end to
end. Mute, deafen, device and volume control, a speaking threshold, and a
working microphone test with a live level bar.

</td><td width="33%" valign="top">

**Reading**

Message history, live messages, edits and deletes. Images, GIFs, custom emoji,
stickers and link previews, animated ones included. Click any picture for a
full size viewer with scroll to zoom and drag to pan.

</td><td width="33%" valign="top">

**Everything else**

Email and password sign-in with second factor. Server folders you can drag and
rename. A Friends page. Full profile windows with banners, badges, roles and
mutuals. Five plugins with their own settings.

</td></tr>
</table>

Looks and behaves like the real client in these ways:

- Server rail with round icons, initials when a server has none, **animated**:
  icons morph from circle to rounded square and slide out an accent pill
- Channel sidebar grouped under categories, hand drawn `#` and speaker marks,
  text channels above voice ones
- Voice channels list whoever is sitting in them, with chat, invite and
  settings buttons on hover; double click a row to join or leave
- A **Friends** page with Online, All, Pending and Blocked tabs, search, and
  Message and Profile buttons on every row
- Direct messages show each person's picture, an online bubble, and what they
  are playing
- **Server folders**: drag to reorder, right click to make one, rename it, move
  servers in and out, or take it apart
- Messages from the same person within seven minutes join one block
- Click any name or avatar for the full profile: banner, large avatar with a
  status bubble, badges, About Me, Member Since, Friends Since, coloured role
  pills, connected accounts, your private note, and mutual friends and servers

## The look

A drawn black hole, with night ink laid over it.

The window is not a flat colour. `AuroraWidget` renders the hole behind
everything: a shadow nothing escapes, the photon ring hard against its edge,
and the disk bending over the top because light from the far side is pulled
around toward you. Every surface above it is glass, tinted slightly toward the
accent, never pure black and never pure white.

| | |
| --- | --- |
| **One seed colour** | Pick a colour and the whole application is derived from it at runtime — surfaces, accents, every stylesheet token, and the hole's own disk. No rebuild. Six presets ship: Singularity, Ember, Ocean, Violet, Jade and Gold. |
| **Depth comes from layers** | The rail, the sidebar and the conversation each sit a real step lighter than the one behind, so they read as separate planes without a single border. |
| **Colour is information** | The status dots are the one thing the seed never touches. Green, yellow and red have to keep meaning online, away and busy whatever else changes. |

```
#07090e  the ground, and the hole      #eef4fb  text
#0a0d14  rail                          #9aa6bc  secondary text
#10151f  sidebar                       #6b768c  timestamps and hints
#151b28  the conversation              #6ee7d8  the singularity: selection, links
#1c2433  the composer, lifted          #8aa4ff  moonlight: badges, connecting
#273044  hover, the only surface
         that moves                    #3dd68c  online, and the speaking ring
                                       #e0b35c  idle
                                       #ff6b7a  busy, errors, deletions
```

Colours live as live buffers in [`src/ui/Theme.h`](src/ui/Theme.h) rather than
constants, which is what lets `applySeed()` retint a running application:
every `QColor(Theme::Accent)` and every `@accent` token reads the new value the
next time it is drawn.

The title bar is painted too. It belongs to Windows rather than Qt, so it is
coloured through `DwmSetWindowAttribute` rather than replaced with a hand made
one — that keeps snap layouts, the system menu and double click to maximise,
and still leaves no seam across the top.

## How a call actually works

Getting sound in and out took four separate fixes, each of which looked like
the whole problem at the time. They are written down because every one of them
presents as plain silence.

**1. The voice gateway is v8, and the port must be kept.**
`VOICE_SERVER_UPDATE` hands back `host:port`. Dropping the port reaches a
different machine, which answers, and then closes with `4006`. That reply looks
like progress and is not.

**2. Discord requires end to end encryption on every call.**
`libdave` does the MLS work. Its recognised-user list **must include your own
id**, because you are a member of the group too. Leaving yourself out is
refused with *"Welcome message lists unrecognized user ID"* — and only when you
join a channel that already has somebody in it, which is why an empty channel
seems to work fine.

**3. In the `rtpsize` modes the RTP extension body is encrypted.**
Only the fixed header, the CSRCs and the four byte extension preamble are in
the clear. Counting the extension elements as header makes every packet fail to
open, and Discord sets that bit on nearly all of them.

**4. Playback needs a jitter buffer and a real mixer.**
Qt does not mix. Two streams written to one `QAudioSink` are queued, not
blended, which is heard as chopped and rushed speech. Each speaker gets a
queue, and a 20 ms tick sums them into one frame, with Opus filling in packets
that never arrived.

> If a call is ever silent again, the log prints a tally every five seconds —
> `sent N, played N`, plus a named reason for every frame thrown away. That line
> exists because reading the log found in one run what five rounds of theorising
> could not.

## How video actually works

Cameras and shared screens are two different problems that look like one.

**A camera rides the voice connection.** Sound and pictures share the same
socket and are told apart by the payload type in each packet. But Discord
sends none of it unless you ask: the identify carries a `video` field meaning
*this connection supports video*, which defaults to false, and a client that
leaves it false is promising never to be sent a picture. Then opcode 15 says
which streams are wanted and how good they should be, where zero means do not
send this at all.

**A shared screen does not.** Go Live has its own server, its own websocket and
its own packets, and the voice connection has to stay up underneath it so you
remain in the call. Asking for one is opcode 20 on the main gateway with a key
naming the stream — `guild:<guild>:<channel>:<user>` — and the answer arrives
as two events, exactly like voice.

**A frame is far too big for one packet**, so H.264 is chopped up on the way
out: a whole part alone, several bundled together, or one part split across
many. The last packet of a picture is flagged, which is how we know it is
finished. Each sender needs their own decoder, because a decoder holds the
earlier frames that later ones are described as changes from.

**Loss is the part that decides whether it works.** A decoder that misses a
frame cannot draw anything until a keyframe arrives, and the sender will not
send one unless asked, so a picture loss indication goes back when a frame
cannot be rebuilt. Without that the tile simply stays black for ever.

> The log counts packets, finished pictures, and frames spent waiting on a
> keyframe, per sender. A black tile otherwise cannot say which of those it is.

## Build

```powershell
.\build.ps1
```

| Flag | What it does |
| --- | --- |
| *(none)* | Release build |
| `-Debug` | Debug build |
| `-Clean` | Wipe the build folder first |

Two folders come out of this, and keeping them apart is the point:

| | |
| --- | --- |
| `build\` | Everything the compiler needs and nobody else ever opens |
| `dist\` | The program, and only the program |

Run `dist\Singularity.exe`. Qt's plugins live in one `plugins` folder rather
than the eight it wants by default, which `qt.conf` arranges; that and leaving
out the Visual C++ installer is most of the difference between twenty four
items in there and forty two.
Requires **Qt 6.10.3 (msvc2022_64)** and **Visual Studio 2022**.

> **The running program locks its own file.** A build while Singularity is open fails
> with `LNK1104: cannot open file 'Singularity.exe'`. Close it first.

### Encrypted voice, a one off

Since 1 March 2026 Discord accepts **only** end to end encrypted calls, so
joining any voice channel needs its DAVE library. Everything else in Singularity
builds and runs without it.

```powershell
cd third_party
git clone --recurse-submodules https://github.com/discord/libdave.git
.\libdave\cpp\vcpkg\bootstrap-vcpkg.bat
```

Then, from a Visual Studio command prompt in `third_party\libdave\cpp`:

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release ^
 -DVCPKG_MANIFEST_DIR=vcpkg-alts/openssl_3 ^
 -DCMAKE_TOOLCHAIN_FILE=vcpkg/scripts/buildsystems/vcpkg.cmake ^
 -DBUILD_SHARED_LIBS=OFF -DTESTING=OFF ^
 -DVCPKG_TARGET_TRIPLET=x64-windows-static-md ^
 -DVCPKG_TARGET_ARCHITECTURE=x86_64 ^
 -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL
cmake --build build --target libdave --config Release
```

About eight minutes, nearly all of it OpenSSL.

> **The `-md` on the triplet matters.** libdave's own Makefile uses
> `x64-windows-static`, which builds against the static C runtime and cannot be
> linked with Qt. The `-md` form keeps the static libraries but uses the shared
> runtime, matching Qt.

Singularity's CMake finds the result on its own and prints
`end-to-end encrypted voice is available`. Without it Singularity still builds, and
says plainly that calls cannot be joined.

### Video, also one off

Showing somebody's camera or shared screen needs an H.264 decoder. Singularity brings
its own FFmpeg rather than borrowing the one Qt ships, because Qt's comes with
no headers or link libraries and its version moves whenever Qt does.

Download the **LGPL** shared build — linked dynamically, so nothing here
imposes the GPL on this project — and unpack it so that
`third_party/ffmpeg/lib/avcodec.lib` exists:

```powershell
cd third_party
Invoke-WebRequest -Uri "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip" -OutFile ffmpeg.zip
Expand-Archive ffmpeg.zip -DestinationPath .
Rename-Item "ffmpeg-n8.1-latest-win64-lgpl-shared-8.1" ffmpeg
```

CMake prints `video decoding is available` when it finds it, and copies the
runtime beside the exe. Those files are named `avcodec-62` and so on, while
Qt's are `avcodec-61`, so the two never collide. Without FFmpeg everything
else still builds and calls still carry sound.

**`third_party/` is not in this repository.** It is about four gigabytes of
Opus, libsodium, libdave and OpenSSL, all of it fetched or built by the commands
above. A fresh clone will not build until you run them.

## Layout

```
src/
  core/                network and state, no UI
    AppConfig          settings wrapper
    DiscordIdentity    the client fingerprint both transports must share
    AuthClient         password login, then the second factor step
    RestClient         HTTP calls
    GatewayClient      WebSocket, heartbeat, identify, resume
    MessageStore       in-memory mirror of guilds, channels, messages
    TokenStore         the session token, sealed to your Windows account
    VoiceConnection    the second socket: UDP, Opus, encryption, mixing
    DaveSession        end to end encryption, wrapping libdave
  plugin/
    Plugin.h           the hook interface
    PluginHost         owns plugins, runs the hooks
    builtin/           the shipped plugins
  ui/
    Theme              the palette, one place for every colour
    LoginDialog        email, password, second factor, token fallback
    MainWindow         rail, sidebar, messages, composer
    ListDelegates      hand painted rows, so a long list stays fast
    ProfileDialog      the two pane profile window
    SettingsDialog     five sections, including a live mic test
    ImageViewer        full size pictures, zoom and pan
    FriendsPage        the friends list
```

## Where things are saved

| What | Where | Why there |
| --- | --- | --- |
| Sign-in token | `%APPDATA%\Singularity\Singularity\session.dat` | Sealed with your Windows account key. It needs its own file because the settings file silently failed to keep it, which forced a password sign-in every start |
| Settings | `%APPDATA%\Singularity\Singularity.ini` | Every write is read back, and a failure is logged |
| Log | `%APPDATA%\Singularity\Singularity\singularity.log` | Fresh each run, flushed line by line. `Ctrl+L` opens it in the app |

**Server folders are saved on this machine only.** Discord keeps its own
arrangement in a private format Singularity cannot read or write, so the order here
will not match the official client or your phone.

## Settings

`Ctrl+,` opens the settings window.

| Section | What is in it |
| --- | --- |
| My Account | Your avatar and handle, copy your id, log out |
| Voice & Video | Input and output device, volumes, a working mic test with a live level bar, speaking threshold, join muted or deafened |
| Appearance | Message text size, how long a gap still joins messages, how many messages to load, play animated pictures |
| Plugins | Every built-in plugin with its own settings |
| Advanced | Log file path, open the log folder, developer mode |

## Plugins

They are compiled in. A plugin subclasses `Plugin` and overrides what it needs.
A hook returning `false` cancels the action the client was about to take.

| Hook | Purpose |
| --- | --- |
| `onGatewayEvent` | Every dispatch, raw |
| `onOutgoingMessage` | Rewrite the text, or return false to cancel |
| `onBeforeTyping` | Return false to stay silent |
| `onBeforeMessageDelete` | Return false to keep the message on screen |
| `decorateMessageHeader` | Add a tag next to the author name |
| `createSettingsWidget` | Optional page in the Plugins window |

Register a new one in `PluginHost::registerBuiltins()` and add its files to
`CMakeLists.txt`.

### Shipped

| Plugin | Default | What it does |
| --- | --- | --- |
| Full timestamps | on | Exact date and time on every message |
| Message logger | on | Deleted messages stay visible, edits get tagged |
| Quick text | on | `:shrug:` and friends expand on send |
| Silent typing | off | Never sends the typing signal |
| Presence hints | off | Marks people active when they type, post or join voice, even if Discord reports them offline |
| Anonymous | on | Strips tracking codes out of links you send, and can hide your typing and your status |
| Nitro watch | off | Spots gift links the instant they are posted and offers a Claim button |

### On Nitro watch, and why it does not claim by itself

Discord does not catch automated accounts by measuring how fast you click. It
catches them on the shape of the account: a redeem arriving from a session that
never opened the channel, never scrolled, never moved a pointer, and that does
it again next week at four in the morning. A random delay hides none of that,
because none of it is about timing.

Dead codes are also posted deliberately, to see who bites. A person glances at
a suspicious link and moves on. Anything redeeming on its own takes the bait
every time, and marks itself doing it.

So the part worth automating is the part that is slow for a person: **noticing**.
Singularity watches every channel at once, including the ones you are not looking at,
and takes you from "a gift exists" to one button straight away. You are still
ahead of anyone who has to read their messages first. The press stays yours,
which is the part that keeps the account ordinary.

It also refuses to treat a lookalike domain as a gift. `discord.gift` and
`discord.com/gifts` are the only two places a real one lives, and the host is
checked on what actually matched rather than trusted from the pattern, so
`evil.com/discord.gift/abc` is not mistaken for the real thing. Fake links are
reported instead, because that is the actual danger in a channel full of free
Nitro posts.

### On Anonymous

Most of the tracking people worry about in the official client is already
absent from Singularity, not because a plugin switches it off but because the code to
do it was never written. The plugin's page says so plainly rather than claiming
it as a feature:

| Never sent | |
| --- | --- |
| Analytics events | The official client posts to an endpoint called `/science` as you click around. Singularity never calls it. |
| Read receipts | Nothing tells Discord which messages you have looked at, or when. |
| A real fingerprint | The client details sent on sign-in are fixed numbers written into the source, not your actual Windows version, locale or hardware — and they are identical for everyone running Singularity. |
| Session correlation | The fields the official client fills with identifiers linking your sessions together are sent empty. |
| Game detection | Nothing looks at what programs you have open. |
| Third party images | Pictures come only from Discord's own hosts, so a stranger's message cannot make your client contact an address of their choosing. |

What the plugin genuinely does is the part Singularity *does* send and can stop
sending. Three switches, and only the harmless one starts on:

- **Remove tracking codes from links I send** (on). Share links often carry a
  code naming who sent them, so opening one tells the site that you and the
  sender know each other. This takes those out before the message leaves, which
  protects whoever you send it to as much as it protects you.
- **Never tell anyone I am typing** (off).
- **Always appear offline** (off). This hides you from other people. It does
  **not** hide you from Discord, whose servers still know you are connected,
  because you are.

The link cleaner is the one piece of Singularity that rewrites what you say before it
is sent, so it checks itself against six known cases every time it loads. If
any of them comes out wrong it refuses to touch messages for the rest of the
session and says so in the log. A tracking code getting through is a small
harm; a broken link the sender cannot see is a worse one.

**On Presence hints.** It cannot read a hidden status, because Discord sends
nothing for someone who set themselves invisible. It only notices people who
*do* something, in places you can already see. Someone invisible and idle stays
unseen. A guess is drawn as a hollow ring so it never passes for a reported
status. **Off by default, because it works against a choice the other person
made.**

## Not built yet

Roughly in the order they are worth doing.

1. Scroll up to load older messages
2. Reactions, and replies
3. Edit and delete your own messages from the UI
4. Unread marks and mention badges
5. Member list down the right side
6. Right click menus on messages, channels and people
7. Avatar decorations, which are fetched but not drawn
8. The profile "Recent activity" feed, which needs an endpoint we have not
   worked out
9. Search
10. File upload, and drag and drop
11. Emoji picker
12. Threads and forum channels
13. Sending your own camera or screen. Receiving other people's is built;
    sending is the other half and is not
14. Lottie stickers, which are vector animations with no Qt reader
