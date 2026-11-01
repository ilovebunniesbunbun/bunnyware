// Player position calculations
//
// Sets player dot style and pushed to player location buffer, but does not set
// the location.

socket.element.addEventListener("players", event => {
	let data = event.data

	// Abort if no map has been selected yet
	if (global.currentMap == "none") return

	// Reset active player flags before processing new packet
	for (let i = 0; i < 19; i++) {
		if (global.playerPos[i]) {
			global.playerPos[i].active = false;
		}
	}

	let player_nums = []

	if (data.players && data.players.length > 0) {
		let p0 = data.players[0];
		if (p0.extended_data) {
			console.log("[GSI Debug] Player: " + p0.name + " | Wep: " + p0.extended_data.active_weapon + " | Ammo: " + p0.extended_data.active_ammo + " | Keys: " + Object.keys(p0.extended_data));
		} else {
			console.log("[GSI Debug] Player: " + p0.name + " | has NO extended_data at all!");
		}
	}

	// Loop though each player
	for (let player of data.players) {
        
		// Skip processing if they are disconnected
		if (player.disconnected) {
			global.playerPos[player.num].alive = false;
			continue;
		}

		// Get their player element and start building the class
		let playerDot = global.playerDots[player.num]
		let playerLabel = global.playerLabels[player.num]
		let classes = [player.team]

		player_nums[player.num] = true

		// Mark dead players with a cross
		if (player.health <= 0) {
			classes.push("dead")
		}
		else {
			// Make the bomb carrier orange and and a line around the spectated player
			if (player.bomb) classes.push("bomb")
			if (player.active) classes.push("active")
			if (player.flashed > 0.45) classes.push("flashed")

			// If drawing muzzle flashes is enabled
			if (global.config.radar.shooting) {
				let firedNow = false;
				if (!global.playerShootingTimes) {
					global.playerShootingTimes = {};
				}
				
				// Standard: if we have active_weapon and active_ammo inside extended_data (highly reliable CS2 telemetry fallback!)
				if (player.extended_data && typeof player.extended_data.active_ammo !== "undefined" && player.extended_data.active_weapon) {
					let activeWep = player.extended_data.active_weapon;
					let activeAmmo = player.extended_data.active_ammo;
					
					let prev = global.playerAmmos[player.num];
					// Only count as shooting if the weapon didn't change and current ammo is less than previous ammo
					if (prev && prev.weapon === activeWep) {
						if (prev.ammo > activeAmmo) {
							firedNow = true;
							console.log("[Muzzle Flash Debug] Player " + player.name + " fired! Weapon: " + activeWep + ", Ammo: " + prev.ammo + " -> " + activeAmmo);
						}
					}
					// Cache the current active weapon and ammo stats for the next packet comparison
					global.playerAmmos[player.num] = {
						weapon: activeWep,
						ammo: activeAmmo
					};
				} else {
					// Fallback to legacy weapon ammo object iteration
					for (let weapon in player.ammo) {
						if (global.playerAmmos[player.num][weapon]) {
							// They are shooting if there's less ammo in the clip than the packet before
							if (global.playerAmmos[player.num][weapon] > player.ammo[weapon]) {
								firedNow = true;
							}
						}
					}
					// Save the last ammo stats for the next packet
					global.playerAmmos[player.num] = player.ammo;
				}

				if (firedNow) {
					global.playerShootingTimes[player.num] = Date.now();
				}

				// Keep shooting class for 200ms so muzzle flash is beautifully visible to human eyes on single shots
				let isShooting = false;
				let lastShoot = global.playerShootingTimes[player.num];
				if (lastShoot && (Date.now() - lastShoot < 200)) {
					isShooting = true;
				}

				if (isShooting) {
					classes.push("shooting");
				}
			}

			// If damage indicators are enabled
			if (global.config.radar.damage) {
				// If we have less health than last packet we are hurtin'
				if (player.health < global.playerHealths[player.num]) {
					classes.push("hurting")
				}

				// Save the health value for next time
				global.playerHealths[player.num] = player.health
			}

			// Save the position so the main loop can interpolate it
			global.playerPos[player.num].x = global.positionToPerc(player.position, "x", player.num)
			global.playerPos[player.num].y = global.positionToPerc(player.position, "y", player.num)
			global.playerPos[player.num].a = player.angle
			global.playerPos[player.num].z = player.position.z
			global.playerPos[player.num].active = player.active
		}

		// Add all classes as a class string
		let newClasses = classes.join(" ")

		// Check if the new classname is different than the one already applied
		// This prevents unnecessary className updates and CSS recalculations
		if (playerDot.className != "dot " + newClasses) playerDot.className = "dot " + newClasses
		if (playerLabel.className != "label " + newClasses) playerLabel.className = "label " + newClasses
		if (playerDot.style.display =! "") playerDot.style.display = ""
		if (playerLabel.style.display =! "") playerLabel.style.display = ""

		// Set the player alive attribute (used in autozoom)
		global.playerPos[player.num].alive = player.health > 0

		if (global.config.radar.showName == "both") {
			playerLabel.children[0].textContent = player.name.substring(0, global.config.radar.maxNameLength)
		}
		if (global.config.radar.showName == "always") {
			playerLabel.textContent = player.name.substring(0, global.config.radar.maxNameLength)
		}
	}

	// clear disconnected players
	for (let i = 0; i < 19; i++) {
		if (player_nums[i] != true) {
			if (global.playerDots[i].style.display != "none") global.playerDots[i].style.display = "none"
			if (global.playerLabels[i].style.display != "none") global.playerLabels[i].style.display = "none"
		}
	}
})

// On round reset
socket.element.addEventListener("roundend", event => {
	let phase = event.data
	if (!global.playerBuffers) {
		return
	}
	// Go through each player
	for (let num in global.playerBuffers) {
		// Empty the location buffer
		global.playerBuffers[num] = []
		// Reset the player position
		global.playerPos[num] = {
			x: null,
			y: null,
			z: null,
			a: null,
			alive: false
		}

		// Force a re-render on the dot and label, can sometimes bug out in electron
		global.playerDots[num].style.display = "none"
		global.playerDots[num].style.display = "block"
		global.playerLabels[num].style.display = "none"
		global.playerLabels[num].style.display = ""
	}	
})
