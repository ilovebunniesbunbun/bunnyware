// Main loop
//
// Sets player position on screen at +/- 60fps

let cachedConfig = null;

function getCachedConfig() {
	if (cachedConfig) return cachedConfig;
	if (global.config && global.config.radar) {
		const radar = global.config.radar;
		const vert = global.config.vertIndicator || {};
		cachedConfig = {
			playerDotScale: radar.playerDotScale !== undefined ? radar.playerDotScale : 1.0,
			playerSmoothing: radar.playerSmoothing !== undefined ? radar.playerSmoothing : 1,
			projectileSmoothing: radar.projectileSmoothing !== undefined ? radar.projectileSmoothing : 1,
			highestPlayerOnTop: !!radar.highestPlayerOnTop,
			vertIndicatorType: vert.type || "none",
			vertIndicatorScaleDelta: vert.scaleDelta !== undefined ? vert.scaleDelta : 1.0,
			vertIndicatorColorRange: vert.colorRange || []
		};
		// Cache pre-joined color ranges to avoid joining array on every single frame paint
		if (cachedConfig.vertIndicatorColorRange.length >= 3) {
			cachedConfig.bottomColorStr = cachedConfig.vertIndicatorColorRange[0].join(",");
			cachedConfig.mediumColorStr = cachedConfig.vertIndicatorColorRange[1].join(",");
			cachedConfig.topColorStr = cachedConfig.vertIndicatorColorRange[2].join(",");
		}
	}
	return cachedConfig;
}

// Function to be executed before every frame paint
function step() {
	// Force compositor repaint on transparent Linux windows by microscopically
	// toggling the body background alpha on every single frame paint.
	// We use an alpha of 0.00001 and 0.000011, which is completely invisible to human eyes
	// (rounds to 0 in 8-bit color channels), ensuring a truly 100% transparent background,
	// while still forcing WebKitGTK to perform full frame clears and erase all player/projectile trails.
	// Only run this inside the desktop application on Linux, keeping Windows completely untouched!
	const isLinux = navigator.userAgent.toLowerCase().indexOf("linux") > -1;
	const isTauriApp = typeof window.__TAURI__ !== "undefined" || (window.chrome && window.chrome.webview);
	const isApp = (navigator.userAgent.toLowerCase().indexOf(" electron/") > -1) || isTauriApp;
	if (isLinux && isApp && document.body) {
		if (typeof global._bgToggle === "undefined") global._bgToggle = false;
		global._bgToggle = !global._bgToggle;
		document.body.style.setProperty("background-color", global._bgToggle ? "rgba(0, 0, 0, 0.0000000000000011)" : "rgba(0, 0, 0, 0.000000000000001)", "important");
		
		// The container background is kept completely transparent to avoid any visible box borders or faint grey squares!
		let container = document.getElementById("container");
		if (container) {
			container.style.backgroundColor = "transparent";
		}
	}

	const config = getCachedConfig();
	if (!config) {
		// Wait for configuration to load
		window.requestAnimationFrame(step);
		return;
	}

	if (global.playerBuffers != undefined) {
		// Go though every player location buffer
		for (let num in global.playerBuffers) {
			// Scale that can be changed by the vertical indicator
			let scale = config.playerDotScale

			if (global.playerBuffers[num] === undefined) {
				global.playerBuffers[num].x == 0
				global.playerBuffers[num].y == 0
				global.playerBuffers[num].a == 0
				global.playerBuffers[num].z == 0
			}
	
			// If a new player position is available
			if (global.playerPos[num].x != null) {
				// We want to check if the angle value has looped around, so we need a previous value
				if (global.playerBuffers[num][0]) {
					// If the angle used to be small, but is large now
					if (global.playerPos[num].a > 270 && global.playerBuffers[num][0].a < 90) {
						// Move the old value to the other side of the 360 degree circle
						global.playerBuffers[num].forEach(buffer => {buffer.a += 360})
					}
	
					// If the angle used to be large, but is low now
					if (global.playerPos[num].a < 90 && global.playerBuffers[num][0].a > 270) {
						// Move the old value to the other side of the 360 degree circle
						global.playerBuffers[num].forEach(buffer => {buffer.a -= 360})
					}
				}
	
				// Add it to the start of the buffer
				global.playerBuffers[num].unshift({
					x: global.playerPos[num].x,
					y: global.playerPos[num].y,
					a: global.playerPos[num].a
				})
	
				// Limit the size of the buffer to the count specified in the config
				global.playerBuffers[num] = global.playerBuffers[num].slice(0, config.playerSmoothing)
	
				// If the map and config support vertical indicator
				if (global.mapData.zRange && config.vertIndicatorType !== "none") {
					// Get the z range from the config
					let zRange = global.mapData.zRange
	
					// If the player is in a split, get that z range
					if (typeof global.playerPos[num].split == "number") {
						if (global.playerPos[num].split >= 0) {
							zRange = global.mapData.splits[global.playerPos[num].split].zRange
						}
					}
	
					// Calculate player z-height in the given range on a scale of 0 to 1
					let perc = Math.abs(global.playerPos[num].z - zRange.min) / Math.abs(zRange.max - zRange.min)
					if (global.playerPos[num].z < zRange.min) perc = 0
					perc = Math.min(1, Math.max(0, perc))
	
					// If the color indicator is enabled
					if (config.vertIndicatorType == "color" && config.mediumColorStr) {
						// Get all colors from cache
						let bottomColor = config.bottomColorStr
						let mediumColor = config.mediumColorStr
						let topColor = config.topColorStr
	
						// By default we show the bottom color
						let color = bottomColor
	
						// If we're over half of the range we show the top color
						if (perc > 0.5) {
							color = topColor
							perc = (perc - 0.5)
						}
						else {
							perc = 0.5 - perc
						}
	
						// Show the chosen color as a background color
						document.getElementById("height" + num).style.background = `rgb(${mediumColor})`
						// Overlay the middle color with a transparency to blend the colors
						document.getElementById("height" + num).style.boxShadow = `inset 0 0 0 1.5vmin rgba(${color},${perc * 2})`
					}
	
					// If the scale indicator is enabled
					else if (config.vertIndicatorType == "scale") {
						// Scale the dot by height multiplied by the configured delta
						scale *= ((perc - 0.5) / 2 + 1) * config.vertIndicatorScaleDelta
						global.playerLabels[num].style.transform = `scale(${scale}) translate(-50%, 50%)`
					}
	
					if (config.highestPlayerOnTop) {
						global.playerDots[num].style.zIndex = Math.round(global.playerPos[num].z + 2500)
						global.playerLabels[num].style.zIndex = Math.round(global.playerPos[num].z + 2500)
					}
				}
			}
	
			// Take the average of the X, Y and rotation buffers
			let bufferPercX = (global.playerBuffers[num].reduce((prev, curr) => prev + curr.x, 0) / (global.playerBuffers[num].length))
			let bufferPercY = (global.playerBuffers[num].reduce((prev, curr) => prev + curr.y, 0) / (global.playerBuffers[num].length))
			let bufferAngle = (global.playerBuffers[num].reduce((prev, curr) => prev + curr.a, 0) / (global.playerBuffers[num].length))
	
			// Apply the calculated X and Y to the player dot
			global.playerDots[num].style.left = bufferPercX + "%"
			global.playerDots[num].style.bottom = bufferPercY + "%"
			global.playerLabels[num].style.left = bufferPercX + "%"
			global.playerLabels[num].style.bottom = bufferPercY + "%"
	
			// Apply the transformations to the dots
			if (global.playerPos[num].alive) {
				global.playerDots[num].style.transform = `rotate(${bufferAngle - 45}deg) scale(${scale}) translate(-50%, 50%)`
			}
			else {
				global.playerDots[num].style.transform = `rotate(0deg) scale(${config.playerDotScale}) translate(-50%, 50%)`
			}
		}	
	}

	// Go through each active projectile
	for (let id in global.projectilePos) {
		// Add the newest location to the start of the buffer
		global.projectileBuffer[id].unshift(global.projectilePos[id])

		// Limit the size of the buffer to the count specified in the config
		global.projectileBuffer[id] = global.projectileBuffer[id].slice(0, config.projectileSmoothing)

		// Calculate the avarage position over the active buffer
		let bufferPercX = (global.projectileBuffer[id].reduce((prev, curr) => prev + curr.x, 0) / (global.projectileBuffer[id].length))
		let bufferPercY = (global.projectileBuffer[id].reduce((prev, curr) => prev + curr.y, 0) / (global.projectileBuffer[id].length))

		// Set the location of the projectile
		global.projectilePos[id].elem.style.left = bufferPercX + "%"
		global.projectilePos[id].elem.style.bottom = bufferPercY + "%"
	}

	// Wait for next repaint
	window.requestAnimationFrame(step)
}

// Request an update before the next repaint
// Maximizes FPS with the least CPU possible
window.requestAnimationFrame(step)
