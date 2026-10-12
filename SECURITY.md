# Security Policy

## Supported versions

Security fixes target `main` and the latest stable release. Older releases
do not have a separate maintenance branch; upgrade to the latest release.
Released versions are listed in [release records](docs/releases/README.md).

| Version | Security maintenance |
| --- | --- |
| `main` | Best effort development fixes |
| Latest stable release | Supported |
| Older releases | Upgrade to the latest stable release |

## Reporting a vulnerability

Report suspected vulnerabilities privately. If GitHub private vulnerability
reporting is enabled, use this repository's
[Report a vulnerability form](https://github.com/animu-sphere/hydra-lotus/security/advisories/new).
The feature must be enabled in repository settings before that form accepts
reports; adding this policy alone does not enable it.

If the form is unavailable, use a private contact method published on a
maintainer's GitHub profile. If no private method is available, open a public
issue requesting a private security contact **without describing the
vulnerability, its impact, affected inputs or reproduction steps**. Wait for
a private channel before sending details. Do not disclose the vulnerability
in a public issue or pull request.

In the private report, include where possible:

- The affected release or commit and the component involved.
- Operating system, compiler, GPU/driver, Vulkan/Slang and OpenUSD versions,
  as applicable.
- The impact and steps to reproduce, with a minimal input or proof of concept.
- Relevant sanitized diagnostics and any suggested remediation.

Exclude live credentials, third-party secrets and private scene data.

## Handling and disclosure

This project handles reports on a best-effort basis. Maintainers review the
impact, discuss reproduction or missing information with the reporter and
coordinate disclosure and credit as a fix becomes available. Timing depends
on the issue and maintainer availability.

## Scope

The scope covers Lotus's scene/extraction libraries, Vulkan backend and
shaders, Hydra adapter, headless runner and viewport, resource loading,
build/CI/release automation and installed package interfaces.

Report vulnerabilities in OpenUSD, Vulkan implementations, GPU drivers,
compilers, OpenStrata or other third-party dependencies to their upstream
maintainers. A private heads-up is welcome when Lotus's use of a dependency
contributes to the impact.
