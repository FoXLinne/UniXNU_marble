# AppleKeyStoreState

AppleKeyStore state and selector handling.

used with `sheng-aks=1`.

Key wrapping is software: the device key derives from the identity the board returns through `native_aks_device_id`, and the board must return one. This is not hardware data protection. Reference key operations and device lock stay unsupported.

Build and interface details: [documentation](https://unixnu.com/reference/components). Component sources and dependencies: [catalog](https://unixnu.com/reference/components).
