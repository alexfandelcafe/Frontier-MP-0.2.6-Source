-- FrontierMP resource API sketch. Lua runtime arrives after MVP networking.
RegisterCommand("hello", function(player, args)
    print("hello from resource", player, args)
end)
