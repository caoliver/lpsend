local job_prefix, job_suffix
local write_log, write_log_limited


-- Comment out if running mock I/O
local nonce

local function append_invalid_character_list()
   local invalid_list, count = {}, 0
   for i = 0, 255 do
      if lpsend.invalid_character_found(i) then
	 count = count + 1
	 table.insert(invalid_list, i)
      end
   end
   local buf, out = "Aborting job!  Input has forbidden characters: ", { }
   for i = 1, count do
      buf = buf..
	 string.format("0x%02X", invalid_list[i])..
	 (i < count and ', ' or '.')
      if #buf >= 68 then
	 table.insert(out, buf..'\n')
	 buf = "    "
      end
   end
   if #buf > 0 then
      table.insert(out, buf)
   end
   append_notice(table.concat(out))
end

function sendjob(config, timeouts, features, send_report)
   local ustatus_pattern = ustatus_pattern
   local info_status_pattern = info_status_pattern

   do
      local rndfile, err = io.open("/dev/urandom")
      if not rndfile then error(err) end
      nonce = lpsend.base64_encode(rndfile:read(12))
      rndfile:close()
   end

   do
      local job_log =
	 io.open(config.queue_directory..'/'..config.job.log, "a+")

      write_log = function(str)
	 local success, err = job_log:write(str..'\n')
	 if success then
	    success, err = job_log:flush()
	 end
	 if not success then
	    error("Trouble writing job log.  "..err)
	 end
      end
   end

   -- For error reporting where an evil user can spoof the error.
   do
      local log_limit=16
      write_log_limited = function(str)
	 if log_limit > 0 then
	    write_log(str)
	    log_limit = log_limit - 1
	 end
      end
   end
   
   -- write log header
   do
      local job_ident =
	 string.format("%06x", 
		       lpsend.get_next_job_id(config.queue_directory..'/'..
					      config.job.sequence))
	 
	 write_log(table.concat { "\n", job_ident, ": Job Record\nDate: ",
				  os.date(), "\nHF: ",
				  (os.getenv("HF") or "** UNSPECIFIED **") })
   end

   -- Define a PJL/PostScript prefix to output preceding user's job
   do
      local prefix_table = {
	 "\27%-12345X@PJL USTATUSOFF\n@PJL INFO PAGECOUNT",
	 config.use_pjl_status
	    and "@PJL INFO STATUS\n@PJL USTATUS DEVICE = ON"
	    or "@PJL COMMENT",
	 "@PJL JOB\n%!PS" }

      if config.postscript.prefix then
	 table.insert(prefix_table, config.postscript.prefix)
      end   
      for _,setting in ipairs(config.postscript.settings) do
	 local mask,match,pagedevice_entry = unpack(setting)
	 if bit.band(features,mask) == match then
	    table.insert(prefix_table, pagedevice_entry)
	 end
      end
      if config.postscript.suffix then
	 table.insert(prefix_table, config.postscript.suffix)
      end

      job_prefix = table.concat(prefix_table, "\n")
   end

   -- Define a PJL suffix to output after users job
   job_suffix = "\4\27%-12345X"..
      (config.use_pjl_status and "@PJL USTATUSOFF\n" or "")..
      "@PJL ECHO EOJ "..
      nonce..
      "\n@PJL EOJ\n\27%-12345X"

   -- Define characters allowed in the PostScript input stream.
   --
   --   space and graph and DEL but not VT.
   local permit = lpsend.permit_character
   local isspace, isgraph = lpsend.isspace, lpsend.isgraph
   for ch = 0,127 do
      if isgraph(ch) or isspace(ch) then
	 permit(ch)
      end
   end
   permit(0x7F)
   lpsend.forbid_character(0x0B)

   write_log("\nStarted: "..lpsend.time())

   local state, exit_code = "start", 0
   local remaining = config.jabber_limit
   local arguments = { add_to_output = job_prefix,
		       notices_only = not send_report or
			  remaining == 0
   }

   local handle = {}
   setmetatable(handle,
		{ __index =
		  function(t, k) error("No handler for '"..k.."'") end
		})

   odd_readback = "Odd readback from printer.  Aborting job."

   do
      local previous_state
      function out_of_paper(state)
	 arguments.reset_write_stall_timer = true;
	 write_log "Warning: Printer out of paper"
	 whine "is out of paper"
	 mail_alert "Printer ran out of paper.  Your job will be delayed."
	 lpsend.set_timeouts { select_wait = 5000 }
	 previous_state = state
	 return "paper"
      end

      function paper_loaded()
	 arguments.reset_write_stall_timer = true;
	 mail_alert "Your job has been resumed."
	 lpsend.set_timeouts { select_wait = timeouts.select_wait }
	 return previous_state
      end

      function handle.eoj_reached()
	 eoj_stopwatch = lpsend.stopwatch()
	 blather_count = 0
	 lpsend.set_timeouts { drain_time = timeouts.eoj_drain_time }
	 if state == 'paper' then
	    previous_state = 'wait_drain'
	    return state
	 end
	 return state == 'kill' and 'kill' or 'wait_drain'
      end
   end

   local function kill_job(return_code, new_state)
      aborted = true
      exit_code = return_code
      arguments.freeze_input = true
      arguments.clear_buffer = true
      lpsend.set_timeouts { drain_time = timeouts.eoj_drain_time }
      return new_state or "discard"
   end

   local eoj_stopwatch
   local function report_invalid_readback(state, msg)
      if state == "discard" or state == "kill" then return state end
      eoj_stopwatch = lpsend.stopwatch()
      append_notice(odd_readback)
      write_log("Error: Invalid readback"..(msg or ""));
      arguments.add_to_output = job_suffix
      bad_readback = true
      return kill_job(lprng_exit_code.JREMOVE,"kill")
   end

   function handle.posix_error(_, args)
      local message = args[3]..': '..lpsend.errno_to_string(args[2])
      lpsend.syslog(message)
      whine("died: "..message);
      write_log("Error: "..message)
      aborted = true
      exit_code = lprng_exit_code.JFAIL
      append_notice("A system error occured.  Your job is likely lost."..
		    (config.complaints_to and "" or "\n    ERROR: "..message))
      return "exit"
   end

   function handle.not_postscript()
      append_notice "Sorry!  I can only print PostScript files."
      write_log "Error: Not a PostScript job." ;
      arguments.add_to_output = job_suffix
      return kill_job(lprng_exit_code.JREMOVE)
   end

   function handle.input_stalled()
      append_notice "Input stream stalled.  Aborting job."
      write_log "Error: Input stream stalled."
      arguments.add_to_output = job_suffix
      return kill_job(lprng_exit_code.JFAIL, "kill")
   end

   function handle.invalid_data()
      eoj_stopwatch = lpsend.stopwatch()
      invalid_data_encountered = true
      aborted = true
      exit_code = lprng_exit_code.JREMOVE
      return "kill"
   end

   function handle.stray_delimiter(state, msg)
      return report_invalid_readback(state, " - Stray error delimiter")
   end

   function handle.write_stalled()
      eoj_stopwatch = lpsend.stopwatch()
      whine "output has stalled"
      append_notice [[
The printer has stalled; you may have lost the end of your print job.]]
      write_log "Error: Printer has gone flatline"
      return kill_job(lprng_exit_code.JFAIL)
   end

   do
      local suffix_sent, drain_stopwatch
      function handle.readback_drained(state, args)
	 if state == "start" then return "exit" end
	 if state == "paper" then return state  end
	 if state == "discard" or
	    state == "eoj-nonce-seen" then
	    return "exit"
	 elseif state == "wait_drain" or state == "kill" then
	    if not drain_stopwatch then
	       drain_stopwatch = lpsend.stopwatch()
	    end
	    if not suffix_sent then
	       suffix_sent = true
	       arguments.add_to_output = job_suffix
	    end
	    return drain_stopwatch() > timeouts.eoj_drain_time
	       and "exit"
	       or state
	 end
	 error("readback_drained unhandled state: "..state)
      end
   end

   function record_normal_readback(normal_readback)
      if type(normal_readback) == "number" then
	 suppress_report(normal_readback)
      elseif #normal_readback > 0 then
	 if not remaining then
	    append_report(normal_readback)
	 elseif #normal_readback <= remaining then
	    append_report(normal_readback)
	    remaining = remaining - #normal_readback
	 else
	    append_report(string.sub(normal_readback, 1, remaining))
	    arguments.notices_only = true
	    suppress_report(#normal_readback - remaining)
	    remaining = 0
	 end
      end
   end

   local function test_blather()
      if eoj_stopwatch and timeouts.blather_time_limit and
	 eoj_stopwatch() > timeouts.blather_time_limit
      then
	 if blather_count < config.blather_line_limit then
	    blather_count = blather_count + 1
	 else
	    return true
	 end
      end
   end

   local function kill_blather()
      if state == "blather" then return "exit" end
      write_log "Error: printer is blathering"
      whine "is blathering."
      append_notice "Your job has made the printer far too noisy."
      return kill_job(lprng_exit_code.JREMOVE, "blather")
   end

   local stop_recording_readback
   function handle.ps_msg(state, args)
      if test_blather() then return kill_blather() end
      if bad_readback then return state end
      if not stop_recording_readback then record_normal_readback(args[2]) end
      if args[3] then
	 local flush_msg = "Flushing: rest of job"
	 write_log_limited("PostScript: "..
			   string.sub(lpsend.strip_for_logging(args[3]),1,56))
	 append_notice("PostScript: "..args[3])
	 if string.sub(args[3], 1, #flush_msg) == flush_msg then
	    stop_recording_readback = true
	    eoj_stopwatch = lpsend.stopwatch()
	    return kill_job(lprng_exit_code.JREMOVE, "kill")
	 end
      end
      return state
   end

   local previous_state
   function handle.printer_status(state, args)
      -- Printer error condition
      if bit.band(args[2], 8) == 0 then
	 whine "is on fire"
	 write_log "Error: Printer error condition"
	 append_notice(
	    "The printer had an error.  Some or all of your job may be lost")
	 aborted = true
	 exit_code = lprng_exit_code.JFAIL
	 return "exit"
      end
      -- Out of paper
      if bit.band(args[2], 32) ~= 0 and state ~= "paper" then
	 return out_of_paper(state)
      end
      -- Normal?
      if args[2] == 24 and state == "paper" then
	 return paper_loaded()
      end
      -- Shouldn't happen, but trap it anyhow
      write_log("Unexpected lpgetstatus value: "..args[2])
      exit_code = lprng_exit_code.JFAIL
      aborted = true
      return "exit"
   end

   local pjl_handler={}

   local function bad_pjl(state, pjl_string)
      return report_invalid_readback(state,
				     " - invalid pjl: "..
					string.sub(
					lpsend.strip_for_logging(pjl_string),
					1,36))
   end

   local whine_toner
   function handle_status_code(status)
      code=tonumber(code)
      -- Paper out
      if code >= 41000 and code <= 41999 then
	 return out_of_paper(state)
      end
      -- Low toner message
      if code == 10006 and not whine_toner then
	 whine_toner = true
	 whine "is low on toner"
	 write_log "Notice: low toner"
	 mail_alert "This printer is low on toner."
      end
      if code < 20000 or code == 40000 then
	 return state == "paper" and paper_loaded() or state;
      end
      -- PJL syntax error.  How?
      if code < 30000 then
	 return bad_pjl(state, "Bogus PJL report "..code)
      end
      -- All other errors: punt!
      whine("has PJL error: "..code);
      write_log("Error: PJL status = "..code)
      append_notice "Printer error.  Your job is likely lost."
      exit_code = lprng_exit_code.JFAIL
      return "exit"
   end

   function pjl_handler.I(state, pjl_string)
      found, _, count = pjl_string:find("^INFO PAGECOUNT\r\n([0-9]+)\r\n$")
      if found then
	 write_log("PagesBefore: "..count)
	 return state
      end
      found, _, code = pjl_string:find(info_status_pattern)
      if found then return handle_status_code(state) end
      
      return bad_pjl(state, pjl_string)
   end

   function pjl_handler.E(state, pjl_string)
      found, _, token = pjl_string:find(eoj_pattern)
      if not found then
	 return bad_pjl(state, pjl_string)
      elseif token ~= nonce then
	 return state
      end
      lpsend.set_timeouts { drain_time = timeouts.eoj_drain_time }
      return "eoj-nonce-seen"
   end

   function pjl_handler.U(state, pjl_string)
      found, _, code = pjl_string:find(ustatus_pattern)
      if not found then
	 return bad_pjl(state, pjl_string)
      end
      return handle_status_code(state)
   end

   function handle.pjl_msg(state, args)
      if test_blather() then return kill_blather() end
      if bad_readback then return state end
      local pjl_handler = pjl_handler[string.sub(args[2],1,1)]
      if not pjl_handler then return bad_pjl(state, args[2]) end
      return pjl_handler(state, args[2])
   end

   function handle.got_signal()
      eoj_stopwatch = lpsend.stopwatch()
      append_notice "Job interrupted by signal.";
      write_log "Error: Caught signal!";
      return kill_job(lprng_exit_code.JREMOVE, "kill");
   end

   do
      local job_stopwatch
      if timeouts.job_limit then job_stopwatch = lpsend.stopwatch() end
      repeat
	 local result = { lpsend.io_loop(arguments) }
	 --   local old = state
	 if job_stopwatch and job_stopwatch() > timeouts.job_limit then
	    append_notice "Your job ran too long. Goodbye."
	    write_log "Kill: long job"
	    whine "may be hung up on a job"
	    state = kill_job(lprng_exit_code.JREMOVE)
	 elseif result[1] ~= "uel_read" then
	    state = handle[result[1]](state,result)
	 end
	 --   write_log(lpsend.wall_clock)..': ('..old..', '..result[1]..') -> '..state)
      until state == "exit"
   end

   if invalid_data_encountered then
      append_invalid_character_list()
      write_log "Error: Input stream contains invalid characters."
   end

   mail_report()

   write_log((aborted and "Aborted: " or "Finished: ")..lpsend.time())

   os.exit(exit_code)
end
