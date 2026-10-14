.text
.global _ZN7android31GetCodec2PlatformComponentStoreEv
.type _ZN7android31GetCodec2PlatformComponentStoreEv,%function
_ZN7android31GetCodec2PlatformComponentStoreEv:
// The pinned image's factories both return shared_ptr<C2ComponentStore>.
// Tail-call with the AAPCS64 indirect result register x8 intact; no C++ object
// layout or host pointer is invented by this adapter.
 b _ZN7android22GoldfishComponentStore6CreateEv
.section .note.GNU-stack,"",%progbits
