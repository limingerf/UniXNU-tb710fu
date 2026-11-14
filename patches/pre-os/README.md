# Official patch context

`sheng-platform-power-context.patch` is copied unchanged from
`UniXNU/UniXNU-sm8550-common@a9f45dc60c035084b95463781538380f4c3606c9`,
`patches/xnu-12377/sheng-platform-power.patch`.

The official macOS OS series applies `zzz-boot-progress.patch` against the
`bsd_init` context established by this patch. The TB710FU minimal kernel leaves
`XNU_SHENG_PMGR=0`; no SM8550 power implementation is selected or invoked.
This is a documented patch ordering dependency, not SM8650 hardware support.
