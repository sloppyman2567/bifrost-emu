# License scope

- [Unlicense.txt](Unlicense.txt) is an exact copy of the root [LICENSE](../LICENSE).
  It covers original Bifrost emulator code, original Asgard integration code and
  original regression tests, except material carrying a separate upstream license.
- [Apache-2.0.txt](Apache-2.0.txt) is the unmodified
  [Apache Software Foundation license text](https://www.apache.org/licenses/LICENSE-2.0.txt).
  It accompanies reused AOSP material licensed under Apache 2.0, including
  the AOSP-derived generated
  [Settings defaults](../Guest/Android/Managed/Framework/FrameworkSettingsDefaults.java).
- [NOTICE-Android.txt](NOTICE-Android.txt) retains attribution and pinned source
  provenance for the six AOSP inputs used by that generated Settings profile.
  Existing per-file notices and downloaded runtime NOTICE files remain applicable.

Asgard uses real Android libraries and some AOSP-derived source/data. Adding
the Apache text documents those upstream terms; it does not relicense all
original Asgard code. AOSP [uses several licenses](https://source.android.com/docs/setup/contribute/licenses),
with Apache 2.0 preferred for much of its userspace. Bionic, kernel headers,
other bundled libraries and tools must retain their individual terms.

Vendored dependencies, external applications and game images/assets retain
their own copyrights and licenses. The project licenses do not grant rights
to the games depicted in compatibility screenshots. See
[third-party distribution notes](../Docs/THIRD_PARTY.md).
