<div align="center">

<img src="assets/wisp-banner.svg" alt="Wisp" width="100%">

<br>

![C++20](https://img.shields.io/badge/C%2B%2B-20-ededed?style=flat-square&labelColor=0a0a0a)
![Qt 6.10](https://img.shields.io/badge/Qt-6.10-ededed?style=flat-square&labelColor=0a0a0a)
![Windows](https://img.shields.io/badge/platform-Windows-a3a3a3?style=flat-square&labelColor=0a0a0a)
![Voice](https://img.shields.io/badge/voice-end--to--end%20encrypted-3ba55c?style=flat-square&labelColor=0a0a0a)
![Plugins](https://img.shields.io/badge/plugins-compiled%20in-a3a3a3?style=flat-square&labelColor=0a0a0a)

</div>

---

A Discord client written from scratch in C++ with Qt 6. No Electron, no web
view, no plugin folder. The plugins are part of the binary, so there is nothing
on disk for anything else to swap out.

Calls work: you can talk, you can hear, and the audio is end to end encrypted
the way Discord now requires.

## Warning, read this first

Discord does not permit third party clients on a normal user account. Running
this can get the account banned. That risk is accepted here on purpose.

What Wisp does with your credentials:

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

Black and grey, with colour kept back for the few things that carry meaning.
Three rules shape it, and all of them live in
[`src/ui/Theme.h`](src/ui/Theme.h), which is the only file holding a colour.

| | |
| --- | --- |
| **No pure black, no pure white** | White text on `#000` bleeds at its edges — halation — and is tiring to read. The darkest surface is `#0a0a0a` and the brightest text is `#ededed`. |
| **Depth comes from layers** | The rail, the sidebar and the conversation each sit a real step lighter than the one behind, so they read as separate planes without a single border. |
| **Colour is information** | The only coloured things left are the status dots and errors, because green, yellow and red are what tell you somebody is online, away or busy. Making those grey would look tidier and say less. |

```
#0a0a0a  rail, the ground              #ededed  text, and the selected pill
#101010  sidebar                       #a3a3a3  secondary text
#161616  the conversation              #6b6b6b  timestamps and hints
#1e1e1e  the composer, lifted          #8f8f8f  embed edges, passing states
#2a2a2a  hover, the only surface
         that moves                    #3ba55c  online, and the speaking ring
                                       #d9a441  idle
                                       #e05561  busy, errors, deletions
```

Every grey above is neutral — equal red, green and blue — so no surface leans
warm or cool. Links are **underlined** rather than tinted, because in a grey
scheme a colour on its own cannot mark them apart from the words around them.

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

## Build

```powershell
.\build.ps1
```

| Flag | What it does |
| --- | --- |
| *(none)* | Release build |
| `-Debug` | Debug build |
| `-Clean` | Wipe the build folder first |

Output lands at `build\Release\Wisp.exe` with the Qt runtime beside it.
Requires **Qt 6.10.3 (msvc2022_64)** and **Visual Studio 2022**.

> **The running program locks its own file.** A build while Wisp is open fails
> with `LNK1104: cannot open file 'Wisp.exe'`. Close it first.

### Encrypted voice, a one off

Since 1 March 2026 Discord accepts **only** end to end encrypted calls, so
joining any voice channel needs its DAVE library. Everything else in Wisp
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

Wisp's CMake finds the result on its own and prints
`end-to-end encrypted voice is available`. Without it Wisp still builds, and
says plainly that calls cannot be joined.

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
| Sign-in token | `%APPDATA%\Wisp\Wisp\session.dat` | Sealed with your Windows account key. It needs its own file because the settings file silently failed to keep it, which forced a password sign-in every start |
| Settings | `%APPDATA%\Wisp\Wisp.ini` | Every write is read back, and a failure is logged |
| Log | `%APPDATA%\Wisp\Wisp\wisp.log` | Fresh each run, flushed line by line. `Ctrl+L` opens it in the app |

**Server folders are saved on this machine only.** Discord keeps its own
arrangement in a private format Wisp cannot read or write, so the order here
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

### On Anonymous

Most of the tracking people worry about in the official client is already
absent from Wisp, not because a plugin switches it off but because the code to
do it was never written. The plugin's page says so plainly rather than claiming
it as a feature:

| Never sent | |
| --- | --- |
| Analytics events | The official client posts to an endpoint called `/science` as you click around. Wisp never calls it. |
| Read receipts | Nothing tells Discord which messages you have looked at, or when. |
| A real fingerprint | The client details sent on sign-in are fixed numbers written into the source, not your actual Windows version, locale or hardware — and they are identical for everyone running Wisp. |
| Session correlation | The fields the official client fills with identifiers linking your sessions together are sent empty. |
| Game detection | Nothing looks at what programs you have open. |
| Third party images | Pictures come only from Discord's own hosts, so a stranger's message cannot make your client contact an address of their choosing. |

What the plugin genuinely does is the part Wisp *does* send and can stop
sending. Three switches, and only the harmless one starts on:

- **Remove tracking codes from links I send** (on). Share links often carry a
  code naming who sent them, so opening one tells the site that you and the
  sender know each other. This takes those out before the message leaves, which
  protects whoever you send it to as much as it protects you.
- **Never tell anyone I am typing** (off).
- **Always appear offline** (off). This hides you from other people. It does
  **not** hide you from Discord, whose servers still know you are connected,
  because you are.

The link cleaner is the one piece of Wisp that rewrites what you say before it
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
13. Video and screen share
14. Lottie stickers, which are vector animations with no Qt reader
