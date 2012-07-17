function status_inquire(config, timeouts)
   local info_status_pattern = '^INFO STATUS'..STATUS_PATTERN

   local io_config = table.join(timeouts, { printer = config.device,
					    drain_time = 500
					  })
   local err = lpsend.initialize_io(io_config)
   if err == errno.EBUSY then return "in use by another process."
   elseif err == errno.ENOENT then return "not available to the system."
   elseif err then
      return "Unexpected error: "..lpsend.errno_to_string(err)
   end

   local arguments = { add_to_output =
		       "\27%-12345X@PJL INFO STATUS\n@PJL EOJ\n",
		       notices_only = true,
		       freeze_input = true
   }

   local loops_left, state = 10, "comatose"
   local no_paper = "out of paper."
   local jammed = "jammed or otherwise broken."
   repeat
      loops_left = loops_left - 1
      local result = { lpsend.io_loop(arguments) }
      if result[1] == "printer_status" then
	 if bit.band(result[2], 32) ~= 0 then
	    return no_paper
	 elseif bit.band(result[2], 8) == 0 then
	    return jammed
	 elseif (result ~= 24) then
	    return string.format("in an unexpected state.  (%d)", result[2])
	 end
      elseif result[1] == "pjl_msg" then
	 local found, _, code = result[2]:find(info_status_pattern)
	 if found then
	    code = tonumber(code)
	    if code >= 41000 and code < 41999 then
	       state = no_paper
	    elseif code == 10006 then
	       state = "low on toner"
	    elseif code < 20000 then
	       state = "available for printing."
	    else
	       state = string.format("%s  (%d)", jammed, code)
	    end
	 else
	    return "confused."
	 end
      elseif result[1] == "readback_drained" then
	 return state
      end
   until loops_left < 1

   return "comatose"
end
