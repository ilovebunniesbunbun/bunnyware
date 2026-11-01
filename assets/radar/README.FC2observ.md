## Updated and maintained by vacmeharder, now utilizing UDP
>This project is a fork of [FC2observ by Moyo](https://github.com/Moyomo/FC2observ) . This project is now maintained by vacmeharder due to longlasting abscence by Moyo. original description continues..
<div align="center">
	<img src="https://i.imgur.com/8xvbY5H.png" alt="FC2observ logo" /><br /><br />
	<strong>External radar for CS2 using FC2 memory reading</strong><br />
	<span>Easy to read, infinitely resizeable, and with tons of settings</span><br /><br />
	<span>Click here to watch a showcase video on Youtube:</span><br />
	<a href="https://www.youtube.com/watch?v=TPowO8yosZs" target="_blank">
 		<img src="https://i.imgur.com/XpxzAUk.jpg" alt="Showcase Video" width="560" height="315" border="1" />
	</a>
</div>

## Info

This project is a fork of [Boltobserv](https://github.com/boltgolt/boltobserv), an external radar made for casting and spectating of CS2 matches using the Game State Integration (GSI).
When you're actively playing in a match the GSI will not send game data though. That's why this project uses FC2 to read memory from the game and send it to the external radar instead.

If you use FC2 in kernel mode all the memory reading operations are done from the kernel driver so VAC will not be able to detect anything. I personally advise against trying to use this radar on any client anti-cheat. Use at your own risk.

### Clean radar backgrounds

The radar images used are made by [simpleradar](https://readtldr.gg/simpleradar) and [readtldr.gg](https://readtldr.gg/), and are much higher quality and with more exact positioning of walls than the in-game radar.

![](https://i.imgur.com/Pvfi8vx.png)

### Infinitely scalable

Because FC2observ runs as an external application, it can be resized to be whatever size you want, and be moved to any display you want.
Running without window borders enables it to dedicate as much space as possible to the radar.

It can even run in a browser, allowing you to view the radar over the network. This also means that the radar can be added as a browser source in applications like OBS with a transparent background.

### And much more

 - Split maps for upper and lower on Nuke and Vertigo
 - Any radar background color, including full transparency
 - Always-on-top and fixed on-screen positioning
 - Player dot z-height indicators, either by color dot or scale
 - Custom configurable OS-level keybinds
 - Automatic .cfg file installation


## Installation

1. Download the latest .zip form the [releases](https://gitlab.com/vacmeharder/fc2observ/-/releases) page and unzip it.
2. Launch `FC2observ.exe`. You're done!

Please report any bugs or feature requests in the release thread, not here.

## Configuration

Most functions of FC2observ are configurable. To make your own config, go to `/config` and edit the `config.json5` file.

## Special thanks

- [boltgolt](https://github.com/boltgolt)
- [readtldr.gg](https://readtldr.gg/)
- [typedef](https://github.com/typedef-FC/)
- [Valve Software](https://github.com/ValveSoftware)
- [FC2observ by Moyo](https://github.com/Moyomo/FC2observ)
## License

This project is licensed under GPL-3. In short, this means that all changes you make to this project need to be made open source (among other things). Commercial use is encouraged, as is distribution.

The paragraph above is not legal advice and is not legally binding. See the LICENSE file for the full license.