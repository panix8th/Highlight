# Changelog

## 1.0.2

Share link fixes from a review of 1.0.1:
- Uploading a clip again (from the menu or Share) no longer loses its earlier link: earlier links and links of deleted clips are kept as *older links*, and **Settings → Share links** can delete them.
- *Delete link* deletes from the bucket the clip was uploaded to, even after the settings change, and says so when it could not.
- MKV and MOV clips are uploaded as MP4 so their links play in the browser and in Discord.
- Clearer messages: no internet, a connection that breaks off (with a pointer to the Test button), a bucket in the EU (paste its S3 address).
- No long cache header, so a deleted clip stops showing on a custom domain too; the log no longer contains link addresses.
- A cancel at the very end of an upload keeps it; *Try again* in Share waits for a running upload; a test result is only shown for the settings that were tested; renaming or deleting a clip while it uploads keeps its link with the right clip; closing Highlight during an upload no longer leaves a frozen window.

## 1.0.1

- **Share links, like Medal.** Upload a clip to your own Cloudflare R2 storage and get a link on the clipboard that plays in any browser and right inside Discord. From a clip's ··· menu (*Upload & copy link*, *Copy link*, *Delete link*) or with *Upload and copy a link* in Share. Set it up once in **Settings → Share links** (steps in the README).
- Logo: the notch in the right bar has the flat tip and even gap of the artwork; small icons (taskbar, tray, 16–48 px) are pixel-sharp.
- Installer pictures sized for current Inno Setup, so they are no longer stretched.
- The REC dot on the tray icon no longer touches the mark; the window wordmark has even letter spacing.

## 1.0.0

- New logo: slanted H with a play triangle and the "highlight" wordmark, on the orange app icon. Used for the app and tray icons, the in-game banner, the window, the installer and the README.
- Capture the game in front with **Ctrl+F10** when a game is not picked up; Highlight remembers it, and switches to a known or remembered game that comes to the front.
- Minecraft is recognised by its window, also in windowed mode; forced capture survives settings changes.
