// Websocket implementation
//
// Starts a connection to the server and emits events for other scripts to
// listen for.

let socket = {
	native: null,
	connected: false,
	connect: () =>  {
		const connectWebSocket = () => {
			let origin = window.location.origin;
			if (origin.indexOf("localhost") > -1) {
				origin = origin.replace("localhost", "127.0.0.1");
			}
			// Fetch the doorknock to get the socket port
			fetch(origin + "/doorknock")
			.then(resp => resp.json())
			.then(data => {
				// If we are on the waiting screen but the server's activePage is not waiting, reload to transition
				if (document.getElementById("connectbolt") && data.activePage && data.activePage !== "waiting") {
					window.location.replace(window.location.origin + "/");
					return;
				}
				// If we are on the map screen but the server's activePage is waiting, reload to transition
				if (document.getElementById("radar") && data.activePage && data.activePage === "waiting") {
					window.location.replace(window.location.origin + "/");
					return;
				}

				let host = window.location.hostname;
				if (host === "localhost" || !host) {
					host = "127.0.0.1";
				}
				// Start a socket to the port we got from the doorknock
				let websocket = new WebSocket(`ws://${host}:${data.socket}`)
				attachEvents(websocket)

				// If we're on the waiting screen with a version element, insert the version we got
				if (document.getElementById("version") && document.getElementById("version").innerHTML == "") {
					document.getElementById("version").innerHTML = data.version
				}

				// Add a nice line in the console <3
				console.info(`%cFC2observvmh %cv${data.version}%c, at your service 💜 `, "font-weight: bold", "font-weight: bold; color:red", "font-weight: bold")
			})
			.catch((err) => {
				console.error("Failed to connect via WebSocket:", err);

				// If something went wrong in the doorknock, try it again in 25ms
				socket.connected = false
				setTimeout(() => {
					socket.connect()
				}, 25)
			});
		};

		// Connect directly via highly optimized local WebSocket for flawless performance
		connectWebSocket();

		function attachEvents(websocket) {
			// Called when the socket is started
			socket.native = websocket;
			websocket.onopen = () => {
				socket.connected = true
				websocket.send("requestWelcome")
			}

			// Called when the server has pushed a message to us
			websocket.onmessage = event => {
				// Decode the message
				let incom = JSON.parse(event.data)
				if (incom.type === "update") {
					// Dispatch each telemetry block as a local DOM event
					for (let type in incom.data) {
						let outev = new Event(type)
						outev.data = incom.data[type]
						socket.element.dispatchEvent(outev)
					}
				} else {
					// Fallback for welcome and other control packets
					let outev = new Event(incom.type)
					outev.data = incom.data
					socket.element.dispatchEvent(outev)
				}
			}

			// Called when the socket is closed, try opening the socket again in 25ms
			websocket.onclose = event => {
				socket.connected = false
				let outev = new Event("close")
				socket.element.dispatchEvent(outev)
				setTimeout(() => {
					socket.connect()
				}, 25)
			}

			// Called when a handler errors out, close the socket and start clean
			websocket.onerror = err => {
				console.error(err)
				if (socket.native) socket.native.close()
			}
		}
	},

	// Dummy element, events are fired from here
	element: document.createElement("div")
}

// Start the socket
socket.connect()

// On a round indicator packet
socket.element.addEventListener("round", event => {
	if (!event.data) return;
	let phase = event.data.toLowerCase();

	// Abort if there's no change in phase
	if (global.gamePhase == phase) return

	// If the round just reset to freezetime, clear the smokes
	if (phase == "freezetime" && global.gamePhase !== "freezetime") {
		let roundend = new Event("roundend")
		socket.element.dispatchEvent(roundend)
	}

	global.gamePhase = phase
})

socket.element.addEventListener("effect", event => {
	global.effects[event.data.key] = event.data.value
})
