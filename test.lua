local sfinder = require 'sfinder'
local _, message = sfinder.start_jvm(arg[1])
assert(message == nil, message)

local X = true
local _ = false

local t, message = sfinder.percent(4, true, {
    X, _, X, _, _, _, X, _, X, X,
    X, _, X, _, _, _, _, _, X, X,
    X, _, _, _, _, _, _, _, X, _,
    X, _, _, _, _, _, _, _, _, _,
}, {'LTSZIOJ'}, -1, 'softdrop', 'srs')
assert(message == nil, message)
assert(#t == 1)
assert(table.concat(t[1][1]) == 'LTSZIOJ')
assert(t[1][2] == true)

local _, message = sfinder.destroy_jvm()
assert(message == nil, message)
