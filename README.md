<!-- rumdl-disable MD033 MD041 -->
<a id="readme-top"></a>

> [!CAUTION]
> **Read this before you do anything.**
>
> - There is no warranty. If you break your console, that's your fault.
> - It runs on the Nintendo Switch OLED and nothing else, and there are no plans for any other model.
> - There is no support, for installing it or for anything else. Issues are disabled.
> - Don't send me Twitter DMs or emails about it.
> - PRs are accepted, but slop won't be merged. im not reading allat.
> - There's no promise when, or whether, a PR gets merged. i have a life too.

<br />
<div align="center">
  <a href="https://github.com/brunostjohn/ios-on-switch-lmao">
    <img src=".github/logo.svg" alt="ios-on-switch-lmao logo" width="96" height="96">
  </a>

  <h3 align="center">ios-on-switch-lmao</h3>

  <p align="center">
    iPadOS, booting natively on a Nintendo Switch OLED.
    <br />
    <a href="https://ios-on-switch-lmao.brunostjohn.com"><strong>Read the docs »</strong></a>
    <br />
    <br />
    <a href="https://ios-on-switch-lmao.brunostjohn.com/installation">Install it</a>
    &middot;
    <a href="https://ios-on-switch-lmao.brunostjohn.com/what-works">What works</a>
    &middot;
    <a href="https://ios-on-switch-lmao.brunostjohn.com/how-it-works">How it works</a>
  </p>
</div>

<details>
  <summary>Table of Contents</summary>
  <ol>
    <li><a href="#about-the-project">About the project</a></li>
    <li><a href="#getting-started">Getting started</a></li>
    <li><a href="#contributing">Contributing</a></li>
    <li><a href="#sponsors">Sponsors</a></li>
    <li><a href="#license">License</a></li>
    <li><a href="#contact">Contact</a></li>
    <li><a href="#acknowledgments">Acknowledgments</a></li>
  </ol>
</details>

## About the project

<!-- TODO(photo): a photo of the Switch running iOS, e.g.
<img src=".github/photo.jpg" alt="A Switch OLED showing the iPadOS Home Screen" width="720">
-->

iPadOS runs directly on the Switch's hardware. The kernel is Apple's XNU, built from the source Apple publishes, with
drivers for the Switch's chips. Everything above it is Apple's own iPadOS, which your Mac downloads from Apple when it
builds the install image. You start it from hekate, the way people boot Android or Linux on a Switch, and it lives on
its own partition of the SD card.

The docs list [what works and what doesn't](https://ios-on-switch-lmao.brunostjohn.com/what-works) and explain
[how it works](https://ios-on-switch-lmao.brunostjohn.com/how-it-works).

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## Getting started

You need a Switch OLED that already boots hekate, a microSD card, and a Mac with
[Bazelisk](https://github.com/bazelbuild/bazelisk) installed. The
[installation guide](https://ios-on-switch-lmao.brunostjohn.com/installation) has the rest.

Support questions go to the docs. Nobody answers them anywhere else.

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## Contributing

Pull requests are the only contributions accepted. Fill in every section of the PR template, because a check fails PRs
that don't. [Contributing](https://ios-on-switch-lmao.brunostjohn.com/contributing) covers building and testing, and
[AGENTS.md](AGENTS.md) has the rules the code follows, which apply to people as much as to agents.

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## Sponsors

Thank you for your ongoing support. [Click here to become my sponsor.](https://github.com/sponsors/brunostjohn)

<!-- sponsors --><!-- sponsors -->

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## License

The project's own code is licensed under GPL-2.0-only (see [LICENSE](LICENSE)). Upstream code keeps its own licence,
as [NOTICE](NOTICE) explains.

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## Contact

Bruno St John: [@brunostjohn](https://x.com/brunostjohn) on Twitter. Please don't DM me about this project.

Project link: [github.com/brunostjohn/ios-on-switch-lmao](https://github.com/brunostjohn/ios-on-switch-lmao)

<p align="right">(<a href="#readme-top">back to top</a>)</p>

## Acknowledgments

Thanks to these projects and the people behind them:

- [UniXNU](https://github.com/UniXNU/UniXNU) and [UniXNU-qemu](https://github.com/xVanTuring/UniXNU-qemu)
- [hekate](https://github.com/CTCaer/hekate) by CTCaer
- [switchroot](https://gitlab.com/switchroot), including
  [nx-firmware](https://gitlab.com/switchroot/android/nx-firmware) and the
  [L4T kernel](https://github.com/CTCaer/switch-l4t-kernel-4.9)
- [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel)
- [IOS-26-Hot](https://github.com/nithinyadavamaraboina-online/IOS-26-Hot)
- [nullmoth](https://github.com/nullmoth/nvidia-macos-driver)
- [metal2vulkan](https://github.com/steelbrain/metal2vulkan) by steelbrain
- [Mesa](https://mesa3d.org) and its [NVK](https://docs.mesa3d.org/drivers/nvk.html) driver, and
  [nouveau](https://nouveau.freedesktop.org)
- [Linux](https://www.kernel.org) and NVIDIA's [L4T](https://developer.nvidia.com/embedded/jetson-linux)
- [libvpx](https://chromium.googlesource.com/webm/libvpx)
- [Berkeley SoftFloat](https://github.com/ucb-bar/berkeley-softfloat-3)
- André Eikmeyer's
  [brcmfmac AWDL series](https://lore.kernel.org/linux-wireless/?q=brcmfmac+AWDL)<!-- TODO(url): the series' cover letter on lore.kernel.org -->
  (LKML, 2026-11-04) and [KaiT2en](https://github.com/kaiT2en)'s t2-drop<!-- TODO(url): t2-drop's repository -->
- [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)
- [Bergamota](https://github.com/ry-diffusion/Bergamota)
- [ios-sweet](https://github.com/dipa-studio/ios-sweet)
- [switch-frame-tap](https://github.com/TomasUribe/switch-frame-tap) by TomasUribe
- [libdrm](https://gitlab.freedesktop.org/mesa/libdrm), [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo),
  [FFmpeg](https://ffmpeg.org) and [LZ4](https://github.com/lz4/lz4)
- Infineon's [Wi-Fi Host Driver](https://github.com/Infineon/wifi-host-driver)
- Apple, for publishing [XNU's source](https://github.com/apple-oss-distributions/xnu)

<p align="right">(<a href="#readme-top">back to top</a>)</p>

---

Apple, iOS, iPadOS and the Apple logo are trademarks of Apple Inc. Nintendo Switch is a trademark of Nintendo. This
project claims no ownership of them and intends no infringement.
