# Lucy N. — Google Authentication

---

## Overview

Lucy N. implements Google Sign-In using raw OAuth2 + OpenID Connect — no Google SDK, no Play Services, no microG. Google publishes this protocol publicly for exactly this use case. Lucy N. just makes 4 HTTP calls.

Reference: https://developers.google.com/identity/protocols/oauth2

---

## What the User Sees

Google serves the auth UI entirely. Lucy N. just opens the URL.

```
┌─────────────────────────────────┐
│                                 │
│   Sign in with Google           │
│                                 │
│   Lucy N. wants to access:      │
│   ✓ Your email address          │
│   ✓ Your basic profile info     │
│                                 │
│   [ Continue ]   [ Cancel ]     │
│                                 │
└─────────────────────────────────┘
```

Google handles the login, 2FA, and consent screen. Lucy N. handles nothing on that page.

---

## What Lucy N. Gets Back

After the user approves:

```json
{
    "email": "user@gmail.com",
    "name": "Ernest Pease",
    "picture": "https://...",
    "sub": "1234567890"
}
```

That's it. Nothing more is requested unless explicitly added to the scope.

---

## Scopes

```
openid
email
profile
```

Minimum required. Gives back email, name, profile picture, and a unique user ID (`sub`).

---

## The 4 HTTP Calls

### 1. Build the Auth URL and open it

Lucy N. opens the auth URL in the user's real browser — not Lucy N.'s own browser. Google sees a real browser with real cookies and a real fingerprint. No suspicion.

Lucy N. then spins a tiny local HTTP server on a random port to catch Google's redirect, grabs the auth code, kills the server, and exchanges the code silently in C++. The user never has to do anything except log in normally.

### Browser Selection

Lucy N. shows the user two options on first auth setup:

```
┌────────────────────────────────────────┐
│                                        │
│   How should Lucy N. open Google       │
│   Sign-In?                             │
│                                        │
│   ● Use my default browser             │
│                                        │
│   ○ Use a specific browser             │
│     Path: [________________________]   │
│                                        │
│   [ Confirm ]                          │
│                                        │
└────────────────────────────────────────┘
```

**Option 1 — Default browser:**
Lucy N. asks the OS to open the URL. Works on every platform with zero config.

```
Windows → ShellExecute
Linux   → xdg-open
macOS   → open
```

**Option 2 — Specific browser:**
User pastes the full path to any browser executable:

```
C:\Program Files\Google\Chrome\Application\chrome.exe
/usr/bin/firefox
/Applications/Brave Browser.app/Contents/MacOS/Brave Browser
```

Lucy N. stores the choice in engine config and uses it for every future auth request. The user only has to set this once.

The selected browser is only opened for the auth flow. Everything else — scraping, HTTP requests, LNM views — stays inside Lucy N.

Lucy N. then opens this URL in that browser:

```
https://accounts.google.com/o/oauth2/v2/auth
    ?client_id=YOUR_CLIENT_ID
    &redirect_uri=YOUR_REDIRECT_URI
    &response_type=code
    &scope=openid email profile
    &access_type=offline
    &prompt=consent
```

`access_type=offline` — tells Google to also return a refresh token so the user doesn't have to log in every session.

---

### 2. Catch the redirect

When the user approves, Google redirects back to `redirect_uri` with an auth code:

```
YOUR_REDIRECT_URI?code=AUTH_CODE_HERE&scope=...
```

Lucy N.'s engine catches this redirect and extracts the `code`.

---

### 3. Exchange the code for tokens

```
POST https://oauth2.googleapis.com/token

body:
    code=AUTH_CODE_HERE
    client_id=YOUR_CLIENT_ID
    client_secret=YOUR_CLIENT_SECRET
    redirect_uri=YOUR_REDIRECT_URI
    grant_type=authorization_code
```

Google responds with:

```json
{
    "access_token": "...",
    "refresh_token": "...",
    "id_token": "...",
    "expires_in": 3599
}
```

Decode the `id_token` (it is a JWT) to get the user's email, name, and picture.

---

### 4. Refresh the token when it expires

Access tokens expire after ~1 hour. Use the refresh token to get a new one silently:

```
POST https://oauth2.googleapis.com/token

body:
    refresh_token=STORED_REFRESH_TOKEN
    client_id=YOUR_CLIENT_ID
    client_secret=YOUR_CLIENT_SECRET
    grant_type=refresh_token
```

Google responds with a fresh `access_token`. No user interaction needed.

---

### Logout / Revoke

```
POST https://oauth2.googleapis.com/revoke

body:
    token=ACCESS_OR_REFRESH_TOKEN
```

---

## Token Storage

Tokens are stored locally on disk, encrypted. Never in plain text.

- Encryption: AES key derived from a machine-specific identifier
- Storage path: engine's local app data directory
- `client_secret` is obfuscated in the binary — never stored in plain text in source

---

## Why This Helps Cloudflare

Cloudflare scores requests by how human they look. A Lucy N. request carrying a valid Google identity token attached as a header has already been verified as human by Google. Cloudflare trusts Google's verification. Bot score drops significantly.

```
LNM.http.get("https://target.com/data", {
    headers: {
        "Authorization": "Bearer " + storedAccessToken
    }
})
```

---

## Setup Requirements

Before any of this works, the developer must:

1. Create a project in **Google Cloud Console**
2. Enable the **Google Identity** API
3. Create **OAuth 2.0 credentials** — get a `client_id` and `client_secret`
4. Add the app's `redirect_uri` to the allowed list

This is free. Google provides credentials to any developer.

---

## Unverified App Warning

Until the app goes through Google's verification process, users will see:

```
⚠️ Google hasn't verified this app
```

For internal or personal tools this is acceptable. For public distribution, Google's verification process (paperwork, not code) must be completed.

---

## LNM Integration Example

```lnm
grab(./login.html)
grab.js(./auth.js)

#google-btn:onclick {
    runjs: startGoogleAuth()
}
```

```js
LNM.register({ startGoogleAuth })

async function startGoogleAuth() {
    const result = await LNM.auth.google()

    if (result.ok) {
        LNM.log.success("Signed in as " + result.email)
        LNM.switch.replace("./dashboard.html")
    } else {
        LNM.log.error("Auth failed: " + result.error)
    }
}
```

`LNM.auth.google()` is the engine-level call that handles steps 1–3 above internally. JS only sees the result.

---

## Warnings & Common Sense

**Google can revoke your token anytime they want.**
No notice. No warning. No appeal process. If Google's systems decide your `client_id` or the account looks suspicious, the token dies and the user has to re-authenticate. This is Google's platform — you are a guest on it. Build accordingly. Always handle auth failures gracefully, always have a re-auth fallback, never assume a token is permanent.

**Do not scrape sites while signed in with the user's Google account.**
This is not a technical limitation — it is a clarity of purpose issue. The Google token exists for one reason: to move Cloudflare's trust score from 0 to roughly 5. That's it. Getting from 5 to 8 or 9 is the job of the engine's fingerprint spoofing, header rotation, and browser behaviour simulation — not the Google identity. The moment you start hammering targets with the user's real Google email attached to every request, you are one abuse report away from that account being flagged, the token being revoked, and potentially the user's Google account being suspended. That is a user trust problem, not just a technical one.

**The correct mental model:**
```
Google auth  →  proves humanity to Cloudflare  →  trust score: ~5
Engine spoofing  →  looks like a real browser  →  trust score: 8-9
```

These are two separate systems doing two separate jobs. Do not collapse them into one.

---

*Lucy N. Google Authentication — Ernest Tech House*