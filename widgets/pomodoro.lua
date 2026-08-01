-- pomodoro.lua - 番茄钟
name = "番茄钟"
useCustomStyle = true
bottomBarHover = false

bg = 0xFFFFFF
border = 0xD0D0D0
alpha = 0.98
gradientEndA = 0.0

local DEFAULT_WORK_COLOR   = 0xFF6347
local DEFAULT_BREAK_COLOR  = 0x4ECDC4
local DEFAULT_TRACK_COLOR  = 0xE8E8E8

local TXT_DARK   = 0x333333
local TXT_MUTED  = 0x999999
local BTN_PAUSE  = 0xFFB347

-- ---- timer management ----

function updateTickTimer()
    local state = storage.get("state") or "idle"
    if state == "work" or state == "break" then
        widget.setTimer("tick", 1000, true)
    else
        widget.cancelTimer("tick")
    end
end

function onVisible()
    updateTickTimer()
end

function onHidden()
    widget.cancelTimer("tick")
end

function onTimer(name)
    -- 宿主会在定时器回调后自动刷新当前组件。
end

-- ---- config ----

function loadConfig()
    workMin           = tonumber(storage.get("workMin"))           or 25
    breakMin          = tonumber(storage.get("breakMin"))          or 5
    longBreakMin      = tonumber(storage.get("longBreakMin"))      or 15
    longBreakInterval = tonumber(storage.get("longBreakInterval")) or 4
    workColor         = tonumber(storage.get("workColor"))         or DEFAULT_WORK_COLOR
    breakColor        = tonumber(storage.get("breakColor"))        or DEFAULT_BREAK_COLOR
    trackColor        = tonumber(storage.get("trackColor"))        or DEFAULT_TRACK_COLOR
    autoStart         = storage.get("autoStart") == "1"

    local savedBg = tonumber(storage.get("bgColor"))
    if savedBg then bg = savedBg end
    local savedBorder = tonumber(storage.get("borderColor"))
    if savedBorder then border = savedBorder end
    followPersonalization = storage.get("followPersonalization") == "1"
end

-- ---- time helpers ----

function timeNow()
    local t = sys.getTime()
    return t.hour * 3600 + t.min * 60 + t.sec
end

function elapsedSince(start)
    local now = timeNow()
    if now < start then now = now + 86400 end
    return now - start
end

function getState()           return storage.get("state") or "idle" end
function getPausedRemaining() return tonumber(storage.get("pausedRemaining")) or 0 end
function getSessions()        return tonumber(storage.get("sessions")) or 0 end

function targetForState(st)
    if st == "work" then
        return workMin * 60
    elseif st == "break" then
        local sessions = getSessions()
        if sessions > 0 and sessions % longBreakInterval == 0 then
            return longBreakMin * 60
        else
            return breakMin * 60
        end
    end
    return workMin * 60
end

function targetSeconds() return targetForState(getState()) end

function remainingSeconds()
    local s = getState()
    if s == "idle"    then return workMin * 60 end
    if s == "paused"  then return getPausedRemaining() end
    local rem = targetSeconds() - elapsedSince(tonumber(storage.get("startTime")) or 0)
    return rem > 0 and rem or 0
end

function progress()
    local s = getState()
    if s == "idle" then return 0 end
    local t = targetSeconds()
    if t == 0 then return 0 end
    local p = 1 - remainingSeconds() / t
    return p > 1 and 1 or p
end

function formatTime(sec)
    local m = math.floor(sec / 60)
    local s = math.floor(sec % 60)
    return string.format("%02d:%02d", m, s)
end

function sessionsInSet() return getSessions() % longBreakInterval end

-- ---- daily statistics ----

function todayStr()
    local ok, d = pcall(os.date, "%Y-%m-%d")
    if ok and d then return d end
    return "today"
end

function getDailyStats()
    local savedDate = storage.get("statsDate") or ""
    local today = todayStr()
    if savedDate ~= today then
        storage.set("statsDate", today)
        storage.set("statsFocusSec", "0")
        storage.set("statsSessions", "0")
    end
    return {
        focusSec = tonumber(storage.get("statsFocusSec")) or 0,
        sessions = tonumber(storage.get("statsSessions")) or 0,
    }
end

function addFocusTime(sec)
    local stats = getDailyStats()
    stats.focusSec = stats.focusSec + sec
    storage.set("statsFocusSec", tostring(math.floor(stats.focusSec)))
end

function addDailySession()
    local stats = getDailyStats()
    stats.sessions = stats.sessions + 1
    storage.set("statsSessions", tostring(stats.sessions))
end

function formatDuration(totalSec)
    local h = math.floor(totalSec / 3600)
    local m = math.floor((totalSec % 3600) / 60)
    if h > 0 then
        if m > 0 then return h .. "h " .. m .. "m" end
        return h .. "h"
    end
    return m .. "m"
end

-- ---- state transitions ----

function checkTransition()
    local s = getState()
    if s == "idle" or s == "paused" then return end
    if remainingSeconds() > 0 then return end

    if s == "work" then
        local sessions = getSessions() + 1
        storage.set("sessions", tostring(sessions))
        addFocusTime(targetForState("work"))
        addDailySession()
        storage.set("state", "break")
        storage.set("startTime", tostring(timeNow()))
        widget.setTitle("番茄钟 - 休息")
        sys.notify("番茄钟", "专注完成！休息一下吧")
    elseif s == "break" then
        if getSessions() >= longBreakInterval then
            storage.set("sessions", "0")
        end
        if autoStart then
            storage.set("state", "work")
            storage.set("startTime", tostring(timeNow()))
            widget.setTitle("番茄钟 - 专注")
            sys.notify("番茄钟", "休息结束，自动开始专注")
        else
            storage.set("state", "idle")
            storage.set("startTime", "0")
            widget.setTitle("番茄钟")
            sys.notify("番茄钟", "休息结束，准备下一轮专注")
        end
    end
    updateTickTimer()
end

-- ---- actions ----

function actionStart()
    storage.set("state", "work")
    storage.set("startTime", tostring(timeNow()))
    storage.set("pausedRemaining", "0")
    widget.setTitle("番茄钟 - 专注")
    updateTickTimer()
end

function actionPause()
    storage.set("prevState", getState())
    storage.set("pausedRemaining", tostring(remainingSeconds()))
    storage.set("state", "paused")
    widget.setTitle("番茄钟 - 暂停")
    updateTickTimer()
end

function actionResume()
    local prevState = storage.get("prevState") or "work"
    local target = targetForState(prevState)
    local pausedRem = getPausedRemaining()
    storage.set("state", prevState)
    storage.set("startTime", tostring(timeNow() - (target - pausedRem)))
    storage.set("pausedRemaining", "0")
    storage.set("prevState", "")
    if prevState == "work" then
        widget.setTitle("番茄钟 - 专注")
    else
        widget.setTitle("番茄钟 - 休息")
    end
    updateTickTimer()
end

function actionStop()
    storage.set("state", "idle")
    storage.set("startTime", "0")
    storage.set("pausedRemaining", "0")
    storage.set("prevState", "")
    widget.setTitle("番茄钟")
    updateTickTimer()
end

function actionSkip()
    local s = getState()
    if s == "work" then
        local elapsed = targetForState("work") - remainingSeconds()
        if elapsed > 0 then addFocusTime(elapsed) end
        local sessions = getSessions() + 1
        storage.set("sessions", tostring(sessions))
        addDailySession()
        storage.set("state", "break")
        storage.set("startTime", tostring(timeNow()))
        widget.setTitle("番茄钟 - 休息")
        sys.notify("番茄钟", "已跳过专注，开始休息")
    elseif s == "break" then
        if getSessions() >= longBreakInterval then
            storage.set("sessions", "0")
        end
        if autoStart then
            storage.set("state", "work")
            storage.set("startTime", tostring(timeNow()))
            widget.setTitle("番茄钟 - 专注")
        else
            storage.set("state", "idle")
            storage.set("startTime", "0")
            widget.setTitle("番茄钟")
        end
        sys.notify("番茄钟", "已跳过休息")
    end
    updateTickTimer()
end

function actionReset()
    storage.set("state", "idle")
    storage.set("startTime", "0")
    storage.set("pausedRemaining", "0")
    storage.set("sessions", "0")
    storage.set("prevState", "")
    widget.setTitle("番茄钟")
    updateTickTimer()
end

-- ---- drawing ----

local btnHit = {}

function drawBtn(x, y, w, h, label, btnColor, txtColor, id)
    draw.rect(x, y, w, h, btnColor, layout.cu(6), 0.90)
    draw.strokeRect(x, y, w, h, txtColor, layout.cu(6), layout.cu(0.8), 0.10)
    local m = draw.measureText(label, layout.fontCu(12), 0, true)
    draw.text(x + (w - m.width) / 2, y + (h - m.height) / 2 + layout.cu(0.5), label, layout.fontCu(12), txtColor, 0, true)
    btnHit[#btnHit + 1] = { id = id, x = x, y = y, w = w, h = h }
end

function drawTrackRing(cx, cy, r, thickness, color, alphaVal)
    local outerR = r + thickness / 2
    local innerR = r - thickness / 2
    draw.circle(cx, cy, outerR, color, alphaVal)
    draw.circle(cx, cy, innerR, bg, 1.0)
end

function drawTickMarks(cx, cy, r, thickness, color, alphaVal)
    local tickLen = layout.cu(4)
    local tickThick = layout.cu(0.7)
    local baseR = r - thickness / 2 - layout.cu(1)
    for i = 0, 3 do
        local a = -math.pi / 2 + i * math.pi / 2
        local x1 = cx + math.cos(a) * (baseR - tickLen)
        local y1 = cy + math.sin(a) * (baseR - tickLen)
        local x2 = cx + math.cos(a) * baseR
        local y2 = cy + math.sin(a) * baseR
        draw.line(x1, y1, x2, y2, tickThick, color, alphaVal * 0.35)
    end
end

function drawGlowArc(cx, cy, r, prog, thickness, color, alphaVal)
    if prog <= 0.002 then return end
    local n = math.max(60, math.floor(prog * 300))
    local sa = -math.pi / 2
    local sweep = prog * 2 * math.pi
    local glowR = thickness * 1.8
    local innerR = r - glowR / 2
    local outerR = r + glowR / 2
    for i = 0, n do
        local a = sa + sweep * i / n
        draw.line(
            cx + math.cos(a) * innerR,
            cy + math.sin(a) * innerR,
            cx + math.cos(a) * outerR,
            cy + math.sin(a) * outerR,
            layout.cu(2.5), color, alphaVal * 0.12)
    end
end

function drawProgressArc(cx, cy, r, prog, thickness, color, alphaVal)
    if prog <= 0.002 then return end
    -- glow layer
    drawGlowArc(cx, cy, r, prog, thickness, color, alphaVal)
    -- main arc
    local n = math.max(120, math.floor(prog * 480))
    local sa = -math.pi / 2
    local sweep = prog * 2 * math.pi
    local innerR = r - thickness / 2
    local outerR = r + thickness / 2
    for i = 0, n do
        local a = sa + sweep * i / n
        draw.line(
            cx + math.cos(a) * innerR,
            cy + math.sin(a) * innerR,
            cx + math.cos(a) * outerR,
            cy + math.sin(a) * outerR,
            layout.cu(1.8), color, alphaVal)
    end
    -- round cap at tip
    local tipAngle = sa + sweep
    local tipX = cx + math.cos(tipAngle) * r
    local tipY = cy + math.sin(tipAngle) * r
    draw.circle(tipX, tipY, thickness / 2, color, alphaVal)
    -- round cap at start
    local startX = cx + math.cos(sa) * r
    local startY = cy + math.sin(sa) * r
    draw.circle(startX, startY, thickness / 2, color, alphaVal * 0.5)
end

function drawDots(cx, cy, filled, total, color, alphaVal)
    local dotR = layout.cu(4.5)
    local gap = layout.cu(14)
    local startX = cx - (total - 1) * gap / 2
    -- connecting line
    if total > 1 then
        draw.line(startX, cy, startX + (total - 1) * gap, cy, layout.cu(0.5), color, alphaVal * 0.12)
    end
    for i = 1, total do
        local dx = startX + (i - 1) * gap
        if i <= filled then
            draw.circle(dx, cy, dotR, color, alphaVal)
        else
            draw.circle(dx, cy, dotR * 0.45, color, alphaVal * 0.22)
        end
    end
end

function currentAccent()
    if getState() == "break" then return breakColor end
    return workColor
end

function stateLabelText()
    local s = getState()
    if s == "work"   then return "专注中" end
    if s == "break"  then return "休息中" end
    if s == "paused" then return "已暂停" end
    return "就绪"
end

function render()
    loadConfig()
    checkTransition()
    btnHit = {}

    local w = layout.width()
    local h = layout.height()
    local cx = w / 2
    local ringCY = h * 0.30

    local s     = getState()
    local rem   = remainingSeconds()
    local prog  = progress()
    local accent= currentAccent()
    local label = stateLabelText()
    local inSet = sessionsInSet()

    local btnW = layout.cu(72)
    local btnH = layout.cu(26)
    local btnGap = layout.cu(10)
    local bottomReserved = layout.cu(20)
    local btnY = h - btnH - bottomReserved

    local ringR     = math.min(w, h) * 0.25
    local ringThick = math.max(layout.cu(5), ringR * 0.14)

    -- track ring
    drawTrackRing(cx, ringCY, ringR, ringThick, trackColor, 0.45)
    -- tick marks at 25% intervals
    drawTickMarks(cx, ringCY, ringR, ringThick, TXT_MUTED, 1.0)
    -- progress arc with glow
    if prog > 0.002 then
        drawProgressArc(cx, ringCY, ringR, prog, ringThick, accent, 1.0)
    end

    -- time display
    local fontSize = math.max(layout.fontCu(20), math.floor(ringR * 0.52))
    local timeStr  = formatTime(rem)
    local tm = draw.measureText(timeStr, fontSize, 0, true)
    draw.text(cx - tm.width / 2, ringCY - tm.height / 2, timeStr, fontSize, TXT_DARK, 0, true)

    -- state label with indicator dot
    local labelY = ringCY + ringR + ringThick + layout.cu(8)
    local sub = ""
    if s == "work" then
        sub = "  " .. (inSet + 1) .. "/" .. longBreakInterval
    elseif s == "break" then
        sub = "  " .. inSet .. "/" .. longBreakInterval
    end
    local fullLabel = label .. sub
    local lm = draw.measureText(fullLabel, layout.fontCu(12), 0, false)
    local labelTextX = cx - lm.width / 2
    -- colored indicator dot before label
    local dotR = layout.cu(3)
    draw.circle(labelTextX - dotR - layout.cu(4), labelY + layout.cu(6), dotR, accent, 0.85)
    draw.text(labelTextX, labelY, fullLabel, layout.fontCu(12), TXT_MUTED, 0, false)

    -- session dots
    local dotsY = labelY + layout.cu(24)
    if dotsY < btnY - layout.cu(20) then
        drawDots(cx, dotsY, inSet, longBreakInterval, accent, 1.0)
    end

    -- daily stats
    local stats = getDailyStats()
    if stats.sessions > 0 then
        local statsText = "今日 " .. stats.sessions .. "轮 · " .. formatDuration(stats.focusSec)
        local sm = draw.measureText(statsText, layout.fontCu(10), 0, false)
        local statsY = dotsY + layout.cu(14)
        if statsY < btnY - layout.cu(10) then
            draw.text(cx - sm.width / 2, statsY, statsText, layout.fontCu(10), TXT_MUTED, 0, false)
        end
    end

    -- buttons
    if s == "idle" then
        local totalW = btnW * 2 + btnGap
        local bx = (w - totalW) / 2
        drawBtn(bx, btnY, btnW, btnH, "开始专注", workColor, 0xFFFFFF, 1)
        drawBtn(bx + btnW + btnGap, btnY, btnW, btnH, "重置", 0xE8E8E8, TXT_MUTED, 10)
    elseif s == "paused" then
        local totalW = btnW * 2 + btnGap
        local bx = (w - totalW) / 2
        drawBtn(bx, btnY, btnW, btnH, "继续", accent, 0xFFFFFF, 2)
        drawBtn(bx + btnW + btnGap, btnY, btnW, btnH, "停止", 0xE8E8E8, TXT_MUTED, 3)
    else
        local totalW = btnW * 2 + btnGap
        local bx = (w - totalW) / 2
        drawBtn(bx, btnY, btnW, btnH, "暂停", BTN_PAUSE, 0xFFFFFF, 4)
        drawBtn(bx + btnW + btnGap, btnY, btnW, btnH, "跳过", 0xE8E8E8, TXT_MUTED, 5)
    end
end

-- ---- input ----

function hitButton(x, y)
    for _, b in ipairs(btnHit) do
        if x >= b.x and x <= b.x + b.w and y >= b.y and y <= b.y + b.h then
            return b.id
        end
    end
    return nil
end

function dispatchButton(id)
    if id == 1  then actionStart()
    elseif id == 2  then actionResume()
    elseif id == 3  then actionStop()
    elseif id == 4  then actionPause()
    elseif id == 5  then actionSkip()
    elseif id == 10 then actionReset()
    end
end

function onMouseDown(x, y, button, delta)
    local bid = hitButton(x, y)
    if bid then dispatchButton(bid) end
end

function onDoubleClick(x, y)
    local bid = hitButton(x, y)
    if bid then dispatchButton(bid) else actionReset() end
end

-- ---- context menu ----

function getContextMenu()
    loadConfig()
    local s = getState()
    local menu = {}

    if s == "idle" then
        menu[#menu + 1] = { id = 1, label = "开始专注", icon = "" }
    elseif s == "paused" then
        menu[#menu + 1] = { id = 2, label = "继续计时", icon = "" }
        menu[#menu + 1] = { id = 3, label = "停止计时", icon = "" }
    else
        menu[#menu + 1] = { id = 4, label = "暂停", icon = "" }
        menu[#menu + 1] = { id = 5, label = "跳过当前阶段", icon = "" }
        menu[#menu + 1] = { id = 3, label = "停止计时", icon = "" }
    end
    menu[#menu + 1] = { separator = true }
    menu[#menu + 1] = { id = 10, label = "重置计数", icon = "" }

    return menu
end

function onMenu(id)
    loadConfig()
    if id == 1  then actionStart()
    elseif id == 2  then actionResume()
    elseif id == 3  then actionStop()
    elseif id == 4  then actionPause()
    elseif id == 5  then actionSkip()
    elseif id == 10 then actionReset()
    end
end

-- ---- settings ----

function applyPreset(wm, bm, lbm, lbi)
    workMin = wm; storage.set("workMin", tostring(wm))
    breakMin = bm; storage.set("breakMin", tostring(bm))
    longBreakMin = lbm; storage.set("longBreakMin", tostring(lbm))
    longBreakInterval = lbi; storage.set("longBreakInterval", tostring(lbi))
end

function imguiRender()
    loadConfig()

    if imgui.collapsingHeader("快捷预设") then
        imgui.text("选择常用时长配置：")
        if imgui.button("经典 25/5") then applyPreset(25, 5, 15, 4) end
        imgui.sameLine()
        if imgui.button("深度 50/10") then applyPreset(50, 10, 30, 4) end
        imgui.sameLine()
        if imgui.button("短促 15/3") then applyPreset(15, 3, 10, 4) end
    end

    if imgui.collapsingHeader("时长设置") then
        local wmText = imgui.inputText("专注时长（分钟）", tostring(workMin))
        local wm = tonumber(wmText)
        if wm and wm > 0 and wm <= 120 and wm ~= workMin then
            workMin = wm; storage.set("workMin", tostring(workMin))
        end

        local bmText = imgui.inputText("短休息（分钟）", tostring(breakMin))
        local bm = tonumber(bmText)
        if bm and bm > 0 and bm <= 60 and bm ~= breakMin then
            breakMin = bm; storage.set("breakMin", tostring(breakMin))
        end

        local lbText = imgui.inputText("长休息（分钟）", tostring(longBreakMin))
        local lb = tonumber(lbText)
        if lb and lb > 0 and lb <= 120 and lb ~= longBreakMin then
            longBreakMin = lb; storage.set("longBreakMin", tostring(longBreakMin))
        end

        local lbiText = imgui.inputText("长休息间隔（轮）", tostring(longBreakInterval))
        local lbi = tonumber(lbiText)
        if lbi and lbi >= 1 and lbi <= 10 and lbi ~= longBreakInterval then
            longBreakInterval = lbi; storage.set("longBreakInterval", tostring(longBreakInterval))
        end
    end

    if imgui.collapsingHeader("行为设置") then
        local newAuto = imgui.checkbox("自动开始下一阶段", autoStart)
        if newAuto ~= autoStart then
            autoStart = newAuto
            storage.set("autoStart", autoStart and "1" or "0")
        end
        imgui.text("开启后，休息结束自动开始专注")
    end

    if imgui.collapsingHeader("颜色设置") then
        local newFp = imgui.checkbox("跟随个性化设置", followPersonalization)
        if newFp ~= followPersonalization then
            followPersonalization = newFp
            storage.set("followPersonalization", followPersonalization and "1" or "0")
        end

        local wc = imgui.colorEdit3("专注颜色", workColor)
        if wc ~= workColor then workColor = wc; storage.set("workColor", tostring(workColor)) end

        local bc = imgui.colorEdit3("休息颜色", breakColor)
        if bc ~= breakColor then breakColor = bc; storage.set("breakColor", tostring(breakColor)) end

        local bgc = imgui.colorEdit3("背景颜色", bg)
        if bgc ~= bg then storage.set("bgColor", tostring(bgc)) end
    end

    if imgui.button("恢复默认设置") then
        for _, k in ipairs({ "workMin", "breakMin", "longBreakMin", "longBreakInterval",
                             "workColor", "breakColor", "trackColor", "bgColor", "borderColor",
                             "followPersonalization", "autoStart" }) do
            storage.remove(k)
        end
        followPersonalization = false
        autoStart = false
    end
end