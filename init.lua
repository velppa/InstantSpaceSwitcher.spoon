--- === InstantSpaceSwitcher ===
---
--- Switch macOS Spaces instantly (no animation) using Option-1..0.
--- Shows current space number with color in the menu bar.
--- Uses iss.so native module loaded in-process for reliable gesture posting.

local obj = {}
obj.__index = obj

obj.name = "InstantSpaceSwitcher"
obj.version = "3.8.1"

-- Load ISS native Lua C module
local issLoader = package.loadlib(os.getenv("HOME") .. "/.local/lib/iss.so", "luaopen_iss")
local iss = issLoader()

-- Pastel colors for each space
local spaceColors = {
   {0.95, 0.45, 0.45},  -- 1: red
   {0.95, 0.60, 0.35},  -- 2: orange
   {0.95, 0.85, 0.35},  -- 3: yellow
   {0.55, 0.85, 0.45},  -- 4: green
   {0.40, 0.80, 0.70},  -- 5: teal
   {0.45, 0.70, 0.95},  -- 6: blue
   {0.55, 0.55, 0.95},  -- 7: indigo
   {0.75, 0.50, 0.90},  -- 8: purple
   {0.90, 0.50, 0.75},  -- 9: pink
   {0.65, 0.65, 0.65},  -- 10: gray
}

-- Pre-generate all icons at load time to avoid repeated canvas create/delete
local cachedIcons = {}
for index = 1, 10 do
   local size = 19
   local canvas = hs.canvas.new({x = 0, y = 0, w = size, h = size})
   local c = spaceColors[index]
   local label = index == 10 and "0" or tostring(index)
   local luminance = 0.299 * c[1] + 0.587 * c[2] + 0.114 * c[3]
   local textColor = luminance > 0.6 and {black = 1} or {white = 1}

   canvas[1] = {
      type = "circle",
      center = {x = size/2, y = size/2},
      radius = size/2 - 1,
      fillColor = {red = c[1], green = c[2], blue = c[3], alpha = 1},
      action = "fill",
   }
   canvas[2] = {
      type = "text",
      frame = {x = 0, y = 1, w = size, h = size},
      text = hs.styledtext.new(label, {
         font = {name = ".AppleSystemUIFontBold", size = 12},
         paragraphStyle = {alignment = "center"},
         color = textColor,
      }),
   }

   cachedIcons[index] = canvas:imageFromCanvas()
   canvas:delete()
end

local function buildMenu(self)
   local info = iss.getSpaceInfo()
   local currentIndex = info and info.current or 0
   local items = {}
   for i = 1, 10 do
      local label = (i == 10) and "Space 10 (⌥0)" or string.format("Space %d (⌥%d)", i, i)
      table.insert(items, {
         title = label,
         checked = (i == currentIndex),
         fn = function() switchToSpace(self, i) end,
      })
   end
   return items
end

local function updateMenuBar(self, index)
   if self._menubar then
      self._menubar:setIcon(cachedIcons[index] or cachedIcons[1], false)
      self._menubar:setTitle("")
   end
end

local function refreshMenuBar(self)
   local info = iss.getSpaceInfo()
   if info then
      updateMenuBar(self, info.current)
   end
end

-- macOS 27 (Golden Gate) ignores the public CGEvent gesture fields, so the
-- legacy synthetic dock-swipe (iss.switchToIndex) is silently dropped. The
-- working method is the *augmented* gesture (iss.switchToIndexAug): it embeds
-- the gesture data in the hidden IOHID struct the system actually reads. It is
-- Dock-driven, so it's instant with a clean menu bar and switches empty spaces
-- — unlike the SkyLight transaction (corrupts the menu bar) and
-- hs.spaces.gotoSpace (no-op on 27), both dead ends here.
local isGoldenGate = hs.host.operatingSystemVersion().major >= 27

local function gotoSpaceByIndex(index)
   local spaceIDs = hs.spaces.spacesForScreen(hs.screen.mainScreen())
   local targetID = spaceIDs and spaceIDs[index]
   return targetID and hs.spaces.gotoSpace(targetID)
end

function switchToSpace(self, index)
   self._lastApp = hs.application.frontmostApplication()
   -- macOS 27: augmented dock-swipe gesture (instant, clean menu bar). Far jumps
   -- pipeline a fast gesture burst in iss.so (~5ms/space, ~50ms for 2→10), with
   -- a feedback top-up for any dropped step. The SLS transaction was a dead end
   -- (mangles the menu bar / desyncs Dock — see notes).
   -- macOS <= 26: legacy dock-swipe.
   local switched
   if isGoldenGate then
      switched = iss.switchToIndexAug(index - 1)
   else
      switched = iss.switchToIndex(index - 1)
   end
   if switched then
      updateMenuBar(self, index)
      return
   end
   -- Fall back to hs.spaces.gotoSpace (animated).
   if gotoSpaceByIndex(index) then
      updateMenuBar(self, index)
   end
end

--- Open a new window of the app that was frontmost before the last space switch.
--- Useful on empty spaces: switch to a new space, then call this to bring your app there.
function obj:newWindowOfLastApp()
   local app = self._lastApp
   if not app then return end
   local currentSpace = hs.spaces.focusedSpace()
   local windowsBefore = {}
   for _, w in ipairs(app:allWindows()) do
      windowsBefore[w:id()] = true
   end
   app:selectMenuItem({"File", "New Window"}, true)
   hs.timer.doAfter(0.3, function()
      for _, w in ipairs(app:allWindows()) do
         if not windowsBefore[w:id()] then
            hs.spaces.moveWindowToSpace(w:id(), currentSpace)
            w:focus()
            return
         end
      end
   end)
end

--- Register as the system HTTP/HTTPS URL handler.
--- Tracks the last-focused Safari window and opens URLs in it,
--- navigating to its space first.
--- Call this before :start() or after; it is idempotent.
function obj:enableURLHandler()
   -- Track last-focused Safari window
   self._lastSafariWindow = nil
   self._safariFilter = hs.window.filter.new("Safari")
   self._safariFilter:subscribe(hs.window.filter.windowFocused, function(win)
      if win then self._lastSafariWindow = win end
   end)

   -- Become the default handler for http and https
   hs.urlevent.setDefaultHandler("http")
   hs.urlevent.httpCallback = function(_scheme, _host, _params, fullURL, _senderPID)
      self:_openURLInLastSafariWindow(fullURL)
   end

   return self
end

function obj:_openURLInLastSafariWindow(url)
   local escaped = url:gsub("\\", "\\\\"):gsub('"', '\\"')

   -- First, look for an existing tab with this URL in any Safari window
   local findScript = string.format([[
      tell application "Safari"
         repeat with w in windows
            set tabIndex to 0
            repeat with t in tabs of w
               set tabIndex to tabIndex + 1
               if URL of t is "%s" then
                  return (id of w as string) & "," & tabIndex
               end if
            end repeat
         end repeat
         return ""
      end tell
   ]], escaped)
   local ok, result = hs.osascript.applescript(findScript)
   if ok and type(result) == "string" and result ~= "" then
      local winIDStr, tabIdxStr = result:match("^(%-?%d+),(%d+)$")
      local winID = tonumber(winIDStr)
      local tabIdx = tonumber(tabIdxStr)
      if winID and tabIdx then
         local existingWin = hs.window.get(winID)
         if existingWin then
            local spaces = hs.spaces.windowSpaces(existingWin)
            if spaces and #spaces > 0 then
               hs.spaces.gotoSpace(spaces[1])
            end
         end
         local activateScript = string.format([[
            tell application "Safari"
               tell window id %d
                  set current tab to tab %d
                  set index to 1
               end tell
               activate
            end tell
         ]], winID, tabIdx)
         hs.osascript.applescript(activateScript)
         return
      end
   end

   local win = self._lastSafariWindow
   if not win or not win:application() then
      hs.urlevent.openURLWithBundle(url, "com.apple.Safari")
      return
   end

   local spaces = hs.spaces.windowSpaces(win)
   if spaces and #spaces > 0 then
      hs.spaces.gotoSpace(spaces[1])
   end

   local winID = win:id()
   local script = string.format([[
      tell application "Safari"
         tell window id %d
            set t to make new tab with properties {URL:"%s"}
            set current tab to t
         end tell
         activate
      end tell
   ]], winID, escaped)
   hs.osascript.applescript(script)
end

function obj:disableURLHandler()
   if self._safariFilter then
      self._safariFilter:unsubscribeAll()
      self._safariFilter = nil
   end
   self._lastSafariWindow = nil
   hs.urlevent.httpCallback = nil
   return self
end

function obj:start()
   -- Menu bar indicator with dropdown
   self._menubar = hs.menubar.new()
   self._menubar:setMenu(function() return buildMenu(self) end)

   -- Hotkeys: Option-1..0 switch to spaces 1..10. Both OS paths are synthetic
   -- dock-swipe gestures (GG = augmented; ≤26 = legacy). The native "Switch to
   -- Desktop N" shortcuts must stay UNBOUND from ⌥N or they'd fight these.
   local keys = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"}
   self._hotkeys = {}
   for i, key in ipairs(keys) do
      local spaceIndex = (i == 10) and 10 or i
      local hk = hs.hotkey.bind({"alt"}, key, function()
         switchToSpace(self, spaceIndex)
      end)
      table.insert(self._hotkeys, hk)
   end

   -- Watch for space changes (swipe, Mission Control, etc.)
   self._spaceWatcher = hs.spaces.watcher.new(function()
      refreshMenuBar(self)
   end)
   self._spaceWatcher:start()

   -- Set initial state
   refreshMenuBar(self)

   return self
end

function obj:stop()
   self:disableURLHandler()
   if self._hotkeys then
      for _, hk in ipairs(self._hotkeys) do
         hk:delete()
      end
      self._hotkeys = nil
   end
   if self._spaceWatcher then
      self._spaceWatcher:stop()
      self._spaceWatcher = nil
   end
   if self._menubar then
      self._menubar:delete()
      self._menubar = nil
   end
   return self
end

return obj
