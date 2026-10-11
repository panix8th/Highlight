# Code signing (SignPath Foundation, free for open source)

Unsigned apps make Windows SmartScreen show "Windows protected your PC". Highlight
uses the **SignPath Foundation** program, which signs open-source projects for free.
The build is already prepared: once the steps below are done, every release
(`[release]` commit or `v*` tag) is signed automatically. Until then releases are
built unsigned, exactly as before.

## 1. Apply (once, by the repository owner)

1. Go to <https://signpath.org> → **Apply** and apply with this repository
   (`github.com/panix8th/Highlight`). Requirements they check: an OSI licence
   (Highlight is GPL-2.0-or-later), a public repository, free downloads, releases
   built by CI from the repository, and a code signing policy on the project page.
   The policy is already in the README ("Code signing policy").
2. Wait for approval. SignPath then gives you an organization in SignPath.io and
   a project with a **release signing policy**.

## 2. Set up the SignPath project

In SignPath.io:

1. **Trusted build system:** link GitHub Actions to the project, following SignPath's GitHub guide.
2. **Artifact configurations:** create two, named exactly `app` and `installer`.

   `app` signs Highlight's own programs inside the portable folder (the bundled OBS
   and FFmpeg files are left as they are):

   ```xml
   <?xml version="1.0" encoding="utf-8"?>
   <artifact-configuration xmlns="http://signpath.io/artifact-configuration/v1">
     <zip-file>
       <pe-file path="Highlight.exe">
         <authenticode-sign />
       </pe-file>
       <directory path="bin/64bit">
         <pe-file path="highlight.exe">
           <authenticode-sign />
         </pe-file>
         <pe-file path="highlight-ui.exe">
           <authenticode-sign />
         </pe-file>
       </directory>
     </zip-file>
   </artifact-configuration>
   ```

   `installer` signs the setup program:

   ```xml
   <?xml version="1.0" encoding="utf-8"?>
   <artifact-configuration xmlns="http://signpath.io/artifact-configuration/v1">
     <zip-file>
       <pe-file path="Highlight-*-setup.exe">
         <authenticode-sign />
       </pe-file>
     </zip-file>
   </artifact-configuration>
   ```

   SignPath may not accept the `*` wildcard. If it doesn't, use the exact file name
   (for example `Highlight-0.1.3-setup.exe`) and update it with each version.

3. Note your **organization ID**, the **project slug**, and the **signing policy slug**
   (usually `release-signing`). Create an **API token** for a CI user that may submit
   signing requests.

## 3. Add the values to GitHub

In the repository: **Settings → Secrets and variables → Actions**

| Kind | Name | Value |
|---|---|---|
| Secret | `SIGNPATH_API_TOKEN` | the API token |
| Variable | `SIGNPATH_ORGANIZATION_ID` | organization ID |
| Variable | `SIGNPATH_PROJECT_SLUG` | project slug |
| Variable | `SIGNPATH_POLICY_SLUG` | only if it isn't `release-signing` |

The next release then goes through SignPath. With the Foundation's release policy, a
person usually has to approve each signing request in SignPath: the build waits up to
two hours for that. After approval, the signed app, zip and installer are published.

## What changes for users

Signed downloads show **SignPath Foundation** as the publisher instead of
"Unknown publisher". SmartScreen builds trust as people download signed releases,
so the warning fades over the first weeks rather than disappearing on day one.
