--- === InstantSpaceSwitcher ===
---
--- Switch macOS Spaces instantly (no animation) using Option-1..0.
--- Shows current space number with color in the menu bar.
--- Uses iss.so native module loaded in-process for reliable gesture posting.

local obj = {}
obj.__index = obj

obj.name = "InstantSpaceSwitcher"
obj.version = "3.2.0"

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

function switchToSpace(self, index)
   self._lastApp = hs.application.frontmostApplication()
   if iss.switchToIndex(index - 1) then
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

function obj:start()
   -- Note: iss.init() event tap is NOT used. Hammerspoon already has
   -- its own event loop; CGEventPost works without a separate tap.

   -- Menu bar indicator with dropdown
   self._menubar = hs.menubar.new()
   self._menubar:setMenu(function() return buildMenu(self) end)

   -- Hotkeys
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
