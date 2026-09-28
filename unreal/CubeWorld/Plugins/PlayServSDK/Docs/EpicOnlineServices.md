# Epic Online Services setup

Epic login ([Authentication](Authentication.md)) needs an Epic Online Services (EOS) product for your title, configured in Epic's Developer Portal, and its credentials registered with PlayServ. Everything below is free; budget about 30 minutes plus DNS propagation. The steps are Epic's, summarised here so this document stands on its own; Epic's documentation is at https://dev.epicgames.com/docs/epic-account-services/getting-started.

## 1. Account and organization

1. The organization owner creates an Epic Games account with a company e-mail at https://www.epicgames.com/id/register and enables **two-factor authentication** (required for the Developer Portal).
2. At https://dev.epicgames.com/portal, sign in and **Create Organization** (type *Enterprise*), accepting the Developer Portal Agreement. That account is the organization's Owner.

## 2. Verify the company domain

Brand settings, and so login, unlock only for a verified domain.

1. Organization name (top-left) → **Organization Management** → **Domains** → **Add Domain**; enter your domain.
2. Epic shows a DNS **TXT** record (`epic-verify-…`). Add it at your DNS provider: type TXT, host `@` (the root domain), the value Epic gave you, TTL 3600. It affects nothing else on the domain.
3. Click **Verify**. Propagation can take minutes to hours (check with https://dnschecker.org).

## 3. Product and client credentials

1. **Create Product** in the portal sidebar; name it after your game. A Dev sandbox and a deployment are created with it (create a deployment under *Sandboxes* if none exists).
2. **Product Settings → Clients → Add New Client Policy**: name it (for example `GameClientPolicy`), type *Custom*, enable **User required**, allow all features and actions.
3. **Add New Client** with that policy. Copy the **Client ID** and **Client Secret** right away: the secret is shown only once.

## 4. Epic Account Services application

1. **Epic Online Services → Epic Account Services**, accept the agreement, **Create Application**.
2. **Brand Settings**: application name, an application website and a privacy-policy URL on your verified domain, any 128×128 logo. **Save draft**; do not submit for review. Draft mode is enough for development and internal testing; Brand Review is only needed before a public launch.
3. **Permissions**: enable **Basic Profile** (mandatory). Others are optional.
4. **Linked Clients**: select the client from step 3.

## 5. Who can sign in during development

In draft mode only Epic accounts that are **members of your organization** can log in. **Organization Management → Members → Invite new** for every developer and tester (role *Developer* is enough); invitations expire after 7 days.

## 6. Where the credentials go

| Credential | Found under | Used by |
|------------|-------------|---------|
| Product ID, Sandbox ID, Deployment ID | Product Settings → SDK Credentials / Sandboxes | your game's EOS plugin configuration (`DefaultEngine.ini`, per that plugin's documentation) |
| Client ID, Client Secret | Product Settings → Clients | your game's EOS plugin configuration **and** PlayServ: **Players → Auth providers → Epic Games** in the admin console, together with the Deployment ID |

PlayServ verifies each Epic access token against the product registered there; until it is registered every Epic login answers `409 Provider 'epic' is not configured`. Test the connection on that page once saved.

---

Part of the PlayServ SDK documentation: [README](../README.md) lists every page.
