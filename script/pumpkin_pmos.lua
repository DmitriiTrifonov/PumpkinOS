-- PumpkinOS on postmarketOS (sxmo-de-sway and other Wayland compositors).
-- The window is fullscreen: its size is the one proposed by the compositor
-- so it follows the orientation of the output (landscape on sxmo when rotated).

function cleanup_callback()
  if pumpkin then
    pumpkin.finish()
  end
end

pit.cleanup(cleanup_callback)

if pit.getenv("WAYLAND_DISPLAY") then
  lib = pit.loadlib("libwwayland")
end

if not lib then
  lib = pit.loadlib("liblsdl2")
end

if not lib then
  print("could not load a display lib")
  pit.finish(0)
  return
end

-- on postmarketOS ALSA is routed to PipeWire by the pipewire-alsa package
pit.loadlib("libaalsa")

pit.mount("./vfs/", "/")

pumpkin = pit.loadlib("libos")
pumpkin.init()

pumpkin.start {
  density    = 144,
  width      = 1024, -- fallback, replaced by the compositor size
  height     = 768,
  fullscreen = true,
  center     = true, -- windows open centered, ignoring the saved position
  appscale   = 150,  -- application windows 1.5x larger (3 screen pixels per point with zoom 2)
  fulllauncher = true, -- the Launcher fills the screen (also scaled by appscale)
  depth      = 16,
  hdepth     = lib.hdepth
}
