# VerifiedRoot

Start with the [adaptation procedure](/porting/platform) for board inputs, source changes, build integration and verification order.

Verified root trust tooling. Builds and checks owner-signed root policies.

| Repository | Maintainer | Scope |
| --- | --- | --- |
| [VerifiedRoot](https://github.com/UniXNU/VerifiedRoot) | [UniXNU](https://github.com/UniXNU) | Verified root trust tooling. Builds and checks owner-signed root policies. |

## State

Used by the owner root chain.

## Build

Build the owner policy and the consuming storage archive using the selected board recipe. Check the image and trust inputs using [integrity and boot policy](/concepts/security), then follow [block integration](/porting/platform#block-and-os-services).
