-- Quick and dirty lua shell in lua.
-- This may prove useful in testing/debugging.

function lua_shell()
   local command, next_line, chunk, error
   repeat
      io.write('Lua> ')
      next_line, command = io.read("*l"), ""
      if next_line then
	 next_line, print_it = next_line:gsub("^=(.*)$", "return %1")
	 repeat
	    command = command..next_line.."\n"
	    chunk, error = loadstring(command, "")
	    if chunk then
	       (function (success, ...)
		   if not success then error = ...
		   elseif print_it ~= 0 then print(...)
		   end end)(pcall(chunk))
	       break
	    end
	    if not error:find('<eof>') then break end
	    io.write('>> ')
	    next_line = io.read("*l")
	 until not next_line
	 if error then print((error:gsub("^[^:]*:(.*)$", "stdin:%1"))) end
      end
   until not next_line
end
