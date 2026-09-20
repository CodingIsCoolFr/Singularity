# Wisp

A small Discord client written in C++ with Qt 6. Plugins are built into the
binary, not loaded from disk.

## Warning

Discord does not permit third party clients on a normal user account. Running
this can get the account banned. That risk is accepted on purpose here.

Your password is sent to `discord.com` and nowhere else, and is never written
to disk or to a log. Only the session token that comes back is kept, sealed
with your Windows account key so no other account on the machine can read it.
The log records the token's length, never the token.

Wisp does not answer captchas. If Discord demands one, the sign-in window says
so and offers the token fallback instead.

## Build

```powershell
.\build.ps1
```

### Encrypted voice (one off)

Since 1 March 2026 Discord accepts **only** end-to-end encrypted calls, so
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

Takes about eight minutes, nearly all of it OpenSSL.

**The `-md` on the triplet matters.** libdave's own Makefile uses
`x64-windows-static`, which builds against the static C runtime and cannot be
linked with Qt. The `-md` form keeps the static libraries but uses the shared
runtime, matching Qt.

Wisp's CMake finds the result on its own and prints
`end-to-end encrypted voice is available`. Without it Wisp still builds, and
says plainly that calls cannot be joined.

| Flag | What it does |
| --- | --- |
| *(none)* | Release build |
| `-Debug` | Debug build |
| `-Clean` | Wipe the build folder first |

Output lands at `build\Release\Wisp.exe` with the Qt runtime beside it.

Requires Qt 6.10.3 (msvc2022_64) and Visual Studio 2022.

## Layout

```
src/
  core/      network and state, no UI
    AppConfig        settings wrapper
    DiscordIdentity  the client fingerprint both transports must share
    AuthClient       password login, then the second factor step
    RestClient       HTTP calls
    GatewayClient    WebSocket, heartbeat, identify, resume
    MessageStore     in-memory mirror of guilds, channels, messages
  plugin/
    Plugin.h         the hook interface
    PluginHost       owns plugins, runs the hooks
    builtin/         the shipped plugins
  ui/
    Theme            the Claude palette, one place for every colour
    LoginDialog      token entry and check
    PluginsDialog    enable, disable, configure
    MainWindow       rail, sidebar, messages, composer
```

## Where things are saved

| What | Where | Why there |
| --- | --- | --- |
| Sign-in token | `%APPDATA%\Wisp\Wisp\session.dat` | Sealed with your Windows account key. It needs its own file because the settings file silently failed to keep it, which forced a password sign-in every start |
| Settings | `%APPDATA%\Wisp\Wisp.ini` | Every write is read back and a failure is logged |
| Log | `%APPDATA%\Wisp\Wisp\wisp.log` | Fresh each run, flushed line by line |

**Server folders are saved on this machine only.** Discord keeps its own
arrangement in a private format Wisp cannot read or write, so the order here
will not match the official client or your phone.

## Settings

`Ctrl+,` opens the settings window. Five sections:

| Section | What is in it |
| --- | --- |
| My Account | Your avatar and handle, copy your id, log out |
| Voice & Video | Input and output device, volumes, a **working** mic test with a live level bar, speaking threshold, join muted or deafened |
| Appearance | Message text size, how long a gap still joins messages, how many messages to load, play animated pictures |
| Plugins | Every built-in plugin with its own settings |
| Advanced | Log file path, open the log folder, developer mode |

The Voice page reads real devices through Qt Multimedia and opens the real
microphone for the test. The devices and volumes it sets are the ones a call
actually uses.

## Plugin hooks

A plugin subclasses `Plugin` and overrides what it needs. A hook returning
`false` cancels the action the client was about to take.

| Hook | Purpose |
| --- | --- |
| `onGatewayEvent` | Every dispatch, raw |
| `onOutgoingMessage` | Rewrite the text, or return false to cancel |
| `onBeforeTyping` | Return false to stay silent |
| `onBeforeMessageDelete` | Return false to keep the message on screen |
| `decorateMessageHeader` | Add a tag next to the author name |
| `createSettingsWidget` | Optional page in the Plugins window |

Register a new plugin in `PluginHost::registerBuiltins()` and add its files to
`CMakeLists.txt`.

## Shipped plugins

| Plugin | Default | What it does |
| --- | --- | --- |
| Full timestamps | on | Exact date and time on every message |
| Message logger | on | Deleted messages stay visible, edits get tagged |
| Quick text | on | `:shrug:` and friends expand on send |
| Silent typing | off | Never sends the typing signal |
| Presence hints | off | Marks people active when they type, post or join voice, even if Discord reports them offline |

**On Presence hints.** It cannot read a hidden status, because Discord sends
nothing for someone who set themselves invisible. It only notices people who
*do* something, in places you can already see. Someone invisible and idle stays
unseen. A guess is drawn as a hollow ring so it never passes for a reported
status. Off by default, because it works against a choice the other person
made.

## Status

Looks and behaves like the real client in these ways:

- Server rail with round server icons, initials when a server has no icon
- Channel sidebar grouped under category headings, with drawn `#` and speaker
  marks, text channels above voice ones
- A **Friends** page above the chats, with Online, All, Pending and Blocked
  tabs, a search box, and Message and Profile buttons on every row
- The direct message list shows each person's picture, an online bubble, and a
  line saying what they are playing or their typed status
- **Server folders**: drag tiles to reorder, right click to make a folder,
  rename it, move servers in and out, or take it apart. Click a folder to open
  or close it
- Click any picture in chat for a **full size viewer**: scroll to zoom around
  the pointer, drag to move, double click for actual size, save, or open in a
  browser
- Voice channels list whoever is sitting in them; click a person for a profile
- Hovering a voice channel shows chat, invite and settings buttons; double
  click the row to join or leave
- Custom emoji, stickers and GIFs all render, and animated ones play
- Link previews (Tenor, YouTube, articles) draw as cards with a coloured edge
- Animated avatars and banners actually animate
- Avatars beside every message, circular, with a coloured initials fallback
- Messages from the same person within 7 minutes join one block
- Pictures show inline, capped so one photo cannot take over the column
- Your own avatar, name and connection state in the bottom left
- Hover and selection are animated: server icons morph from circle to rounded
  square and slide out an accent pill, channel rows fade a panel in behind
- Click any name, avatar, or your own panel for the full profile window:
  banner, large avatar with a status bubble, badges, About Me, Member Since,
  Friends Since, coloured role pills, connected accounts, and your private note
- The profile's right panel has Activity, Mutual Friends and Mutual Servers

Working now:

- Email and password sign-in, with the authenticator app, text message, and
  backup code steps
- Stays signed in between runs; token fallback for when a captcha appears
- Gateway connect, heartbeat, identify, resume, backoff reconnect
- Server list, channel list, direct message list
- Message history, live messages, edits, deletes
- Sending messages, typing indicator both ways
- Small markdown subset: bold, italic, strike, inline code, links
- Plugin host with four built-ins and a settings window
- **Voice calls you can talk and listen in**, end to end encrypted

### How a call actually works

Getting sound in and out took four separate fixes, each of which looked like
the whole problem at the time. They are written down because every one of them
presents as silence.

1. **The voice gateway is v8, and the port must be kept.**
   `VOICE_SERVER_UPDATE` gives `host:port`; dropping the port reaches a
   different machine that answers and then closes with 4006.
2. **Discord requires end-to-end encryption on every call.** `libdave` does the
   MLS work. Its recognised-user list **must include your own id** — you are a
   member of the group too. Leaving yourself out is refused with
   *"Welcome message lists unrecognized user ID"*, which only happens when you
   join a channel that already has people in it.
3. **In the `rtpsize` modes the RTP extension body is encrypted.** Only the
   fixed header, the CSRCs and the 4 byte extension preamble are in the clear.
   Counting the extension elements as header makes every packet fail to open,
   and Discord sets that bit on nearly all of them.
4. **Playback needs a jitter buffer and a real mixer.** Qt does not mix: two
   streams written to one `QAudioSink` are queued, not blended. Each speaker
   gets a queue, and a 20 ms tick sums them into one frame, with Opus filling
   in packets that did not arrive.

If a call is ever silent again, the log prints a tally every five seconds —
`sent N, played N` plus a reason for every frame thrown away.

Not built yet, roughly in the order they are worth doing:

1. Scroll up to load older messages
2. Reactions, and replies
3. Edit and delete your own messages from the UI
4. Unread marks and mention badges
5. Rich embeds (link previews, not just attachments)
6. Member list down the right side
7. Right click menus on messages, channels and people
8. Avatar decorations (the frame around an avatar) are fetched but not drawn
9. The profile "Recent activity" feed, which uses an endpoint we have not
   worked out
10. Search
8. File upload and drag and drop
9. Emoji picker and custom emoji pictures
10. Threads and forum channels
11. Video and screen share
12. Lottie stickers, which are vector animations with no Qt reader
