## Device status
### Generic components
|                                      | JTY D101                                        | Lenovo A369i                | Energy Phone Colors           | Prestigio PAP5500 DUO                           |
|--------------------------------------|-------------------------------------------------|-----------------------------|-------------------------------|-------------------------------------------------|
| DRM                                  | 🟢 OK, needs panel improvements (power up/down) | 🟢 OK, needs sane panel     | 🟡 partial: needs panel fixes | 🟢 OK, needs panel improvements (power up/down) |
| display brightness                   | 🔴 DEAD                                         | 🔴 DEAD                     | 🔴 DEAD                       | 🟢 OK: pwm-backlight                            |
| keypad LED: mediatek,mt6323-led      | 🔴 TBD                                          | 🔴 TBD                      | 🔴 TBD                        | 🟢 OK                                           |
| torch LED: pwm-leds                  | 🔴 TBD                                          | 🔴 TBD                      | 🔴 TBD                        | 🟢 OK: pwm-leds                                 |
| vol +/- keys: mediatek,mt6779-keypad | 🟢 OK                                           | 🔴 TBD                      | 🔴 TBD                        | 🟢 OK                                           |
| power key: mediatek,mt6323-keys      | 🟢 OK                                           | 🟢 OK                       | 🔴 TBD                        | 🟢 OK                                           |
| haptics: regulator-haptic            | 🟢 OK                                           | 🔴 TBD                      | 🔴 TBD                        | 🟢 OK                                           |
| charger                              | 🔴 DEAD                                         | 🔴 DEAD                     | 🔴 DEAD                       | 🔴 DEAD                                         |
| audio (playback + headphone jack)    | 🔴 TBD                                          | 🟡 partial: playback + jack + speaker, no capture | 🔴 TBD                        | 🟡 partial: playback + jack + speaker, no capture |

### Per-device components
|                 | JTY D101                           | Lenovo A369i              | Energy Phone Colors           | Prestigio PAP5500 DUO              |
|-----------------|------------------------------------|---------------------------|-------------------------------|------------------------------------|
| touchscreen     | 🔴 DEAD                            | 🔴 DEAD                   | 🔴 DEAD                       | 🟢 OK: goodix,gt911                |
| panel           | 🟡 partial: needs power cycle seqs | 🟢 OK, needs improvements | 🟡 partial: needs panel fixes | 🟡 partial                         |
| accelerometer   | 🔴 DEAD                            | 🔴 DEAD                   | 🔴 DEAD                       | 🟢 OK: bosch,bma222e (polling)     |
| alsps           | 🔴 DEAD                            | 🔴 DEAD                   | 🔴 DEAD                       | 🟢 OK: rohm,rpr0400 (polling)      |

## Platform status
everything marked with 'needs upstreaming' means it's not existent in the upstream

### CPU
| component | driver                             | status    | note                                    |
|-----------|------------------------------------|-----------|-----------------------------------------|
| SMP       | arch/arm/mach-mediatek/platsmp.c   | 🟢 OK     |                                         |
| cpufreq   | drivers/cpufreq/mediatek-cpufreq.c | 🟢 OK     |                                         |
| hotplug   | arch/arm/mach-mediatek/platsmp.c   | 🟢 OK     | needs upstreaming                       |
| cpuidle   |                                    | 🔴 DEAD   | probably needs new driver, wfi may work |
| PMU       | arm,cortex-a7-pmu                  | 🔴 DEAD   | low priority, should be easy to port    |

### Timer
| component  | driver                | status    | note                      |
|------------|-----------------------|-----------|---------------------------|
| APXGPT     | mediatek,mt6577-timer | 🟢 OK     |                           |
| arch timer | arm,armv7-timer       | 🟢 OK     | needs fix upstreaming     |

### Clocks
all of these need upstreaming

| component | driver                     | status     | note                                 |
|-----------|----------------------------|------------|--------------------------------------|
| topckgen  | mediatek,mt6572-topckgen   | 🟢 OK      |                                      |
| infracfg  | mediatek,mt6572-infracfg   | 🟢 OK      |                                      |
| apmixed   | mediatek,mt6572-apmixedsys | 🟢 OK      |                                      |
| fhctl     | subset of apmixed iirc     | 🔴 DEAD    | not sure if we really need it        |
| mmsys     | mediatek,mt6572-mmsys      | 🟡 partial | some dbi clocks from cg1 are missing |
| mfgcfg    | mediatek,mt6572-mfgcfg     | 🟢 OK      |                                      |
| audio     | mediatek,mt6572-audsys     | 🟡 partial | used as a syscon; the AFE driver maps the regs directly, no dedicated clock driver yet |

### Pinctrl
missing emmc r1r0 pins, needs upstreaming

### Buses
| component | driver                   | status  | note              |
|-----------|--------------------------|---------|-------------------|
| UART      | mediatek,mt6577-uart     | 🟢 OK   |                   |
| I2C       | mediatek,mt6572-i2c      | 🟢 OK   | needs upstreaming |
| SPI       |                          | 🔴 DEAD |                   |
| USB       | mediatek,mtk-musb        | 🟢 OK   |                   |
| USB PHY   | mediatek,generic-tphy-v1 | 🟢 OK   |                   |

### Power
### SoC
| component    | driver                           | status     | note                       |
|--------------|----------------------------------|------------|----------------------------|
| pwrap        | mediatek,mt6572-pwrap            | 🟢 OK      | needs upstreaming          |
| power domain | mediatek,mt6572-power-controller | 🟡 partial | only disp and mfg pds work |

#### PMIC
| component  | driver                    | status    | note                                     |
|------------|---------------------------|-----------|------------------------------------------|
| regulators | mediatek,mt6323-regulator | 🟢 OK     |                                          |
| efuse      | mediatek,mt6323-efuse     | 🟢 OK     | needs upstreaming                        |
| thermal    | mediatek,mt6323-thermal   | 🟢 OK     | needs upstreaming, also tested on mt8163 |
| ADC        | mediatek,mt6323-auxadc    | 🟢 OK     | needs upstreaming, needs cleanup a bit   |
| fuel gauge |                           | 🔴 DEAD   | needs new driver                         |

### Storage
using upstream driver

| component | driver                     | status     | note                                 |
|-----------|----------------------------|------------|--------------------------------------|
| eMMC      | mediatek,mt2701-mmc        | 🟢 OK      | no PMT parser yet                    |
| microSD   | mediatek,mt2701-mmc        | 🟢 OK      |                                      |
| NAND      |                            | 🔴 DEAD    | no known device with NAND            | 

### SoC
misc SoC components without category

| component        | driver                  | status     | note                                                     |
|------------------|-------------------------|------------|----------------------------------------------------------|
| interrupt parent | mediatek,mt6577-sysirq  | 🟢 OK      |                                                          |
| reset controller | mediatek,mt6572-wdt     | 🟢 OK      | needs upstreaming                                        |
| cpu core control | mediatek,mt6572-mcusys  |            | dummy compatible, used for hotplug                       |
| efuse            | mediatek,mt8173-efuse   | 🟢 OK      | needs upstreaming                                        |
| ADC              | mediatek,mt8173-auxadc  | 🟢 OK      |                                                          |
| thermal          | mediatek,mt6572-thermal | 🟡 partial | temperature is higher (?) than actual, needs upstreaming |

### Display
| component        | driver                                                        | status     | note                                                                                                                                    |
|------------------|---------------------------------------------------------------|------------|-----------------------------------------------------------------------------------------------------------------------------------------|
| MMSYS            | drivers/soc/mediatek/mtk-mmsys.c: mediatek,mt6572-mmsys       | 🟡 partial | no cmdq, needs upstreaming and routing table cleanup                                                                                    |
| DRM              | drivers/gpu/drm/mediatek/mtk_drm_drm.c: mediatek,mt6572-mmsys | 🟡 partial | currently falls back to mt2701 plat data, needs own data because we have only 1 rdma                                                    |
| IOMMU            | mediatek,mt6572-m4u                                           | 🟢 OK      | needs upstreaming                                                                                                                       |
| SMI              | mediatek,mt6572-smi-common                                    | 🟢 OK      | needs upstreaming                                                                                                                       |
| LARB             | mediatek,mt6572-smi-larb                                      | 🟢 OK      | needs upstreaming                                                                                                                       |
| overlay          | mediatek,mt6572-disp-ovl                                      | 🟢 OK      | needs upstreaming                                                                                                                       |
| read DMA         | mediatek,mt6572-disp-rdma                                     | 🟢 OK      | needs upstreaming                                                                                                                       |
| write DMA        |                                                               | 🔴 DEAD    | no driver                                                                                                                               |
| BLS              | mediatek,mt2701-disp-pwm                                      | 🟡 partial | brightness doesn't work, needs some fixes and likely new compatible                                                                     |
| color correction | mediatek,mt2701-disp-color                                    | 🟢 OK      |                                                                                                                                         |
| DSI              | mediatek,mt6572-dsi                                           | 🟡 partial | needs upstreaming, vblank timeout before drm inits (observed on mt8163 and mt6735 too), might be compatible with mt2701, needs checking |
| DSI PHY          | mediatek,mt2701-mipi-tx                                       | 🟢 OK      |                                                                                                                                         |
| DBI              |                                                               | 🔴 DEAD    | no known device with DBI display, can't test/port                                                                                       |
| DPI              |                                                               | 🔴 DEAD    | no known device with DPI display, can't test/port                                                                                       |
| hw mutex         | mediatek,mt6572-disp-mutex                                    | 🟡 partial | needs upstreaming, missing mdp ids                                                                                                      |
| CMDQ             |                                                               | 🔴 DEAD    | cmdq is very different from gce, needs lot of drm hacking to make it work                                                               |
| GPU              | arm,mali-400                                                  | 🟢 OK      |                                                                                                                                         |

## External contributions
this is so i can track who contributed to this kernel fork

- [CustomFirmwareDev](https://github.com/gabin8) - i2c dma fix, Prestigio PAP5500 DUO support

## Dead subsystems
there's no upstream support OR they need some effort to make them working

### MDP
| component | similar driver             | note |
|-----------|----------------------------|------|
| read DMA  | mediatek,mt8183-mdp3-rdma  |      |
| resize    | mediatek,mt8183-mdp3-rsz   |      |
| write DMA | mediatek,mt8183-mdp3-wdma  |      |
| sharpness | mediatek,mt8195-mdp3-tdshp |      |

### Camera
non existent in the upstream

### HW video enc/dec
mostly useless i'd say, they're not full hw engines but rather only parts of the process. not worth the effort

### Connectivity
not existent in the upstream

### Audio
| component          | driver / binding             | status  | note                                  |
|--------------------|------------------------------|---------|---------------------------------------|
| AFE (DL1 playback) | mediatek,mt6572-audio        | 🟢 OK   | DL1 playback front-end; no UL/capture |
| sound card         | mediatek,mt6572-mt6323-sound | 🟢 OK   | machine: routing, jack, speaker amp   |
| analog codec       | mediatek,mt6323-sound        | 🟢 OK   | MT6323 PMIC: DAC, headphone, volume   |
| capture (mic)      |                              | 🔴 DEAD |                                       |
| i2s                |                              | 🔴 DEAD |                                       |

### Pericfg
it seems to be clock + reset controller for NAND and USB

### EMI mfd
EMI has performance monitoring + bw limiter

### HACC
might be not worth the effort because it's pretty slow compared to software

### devapc
bus violation monitor? not sure if we really need it, but should be pretty easy to port

### APARM (?)
downstrema calls it APARM_BASE and maps as infrasys? used for watchpoint and breakpoint
