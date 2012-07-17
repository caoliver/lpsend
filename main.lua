local CONFIG_DIR="/etc/printerdefs/"
local VERSION_ID="lpsend 1.0"

local CONFIG_DEFAULTS = {
   blather_line_limit = 250
}
local TIMEOUT_DEFAULTS = {
   eoj_drain_time = 5000,
   blather_time_limit = 20000
}

local config, timeouts

-- Main "verb" for printer configuration file
function define_printer(defn)
   if defn.version_id ~= VERSION_ID then
      error("Invalid configuration version for "..configfile);
   end
   config = table.join(CONFIG_DEFAULTS, defn);
   timeouts = table.join(TIMEOUT_DEFAULTS, config.timeout)
end

do
   -- Strip directory if given.
   local _,_,configname=arg[0]:find("^.*/([^/]*)$")
   if not configname then configname=arg[0] end
   local configfile=CONFIG_DIR..configname
   
   dofile(configfile..'.lpsend')
   define_printer = nil
end

local features, global_options, bad_options = process_options(arg, config)

initialize_mail(global_options, config)

if not features then
   mail_brief("Invalid options to printer ", bad_options)
   os.exit(lprng_exit_code.JREMOVE);
end

do
   local status_msg = "This printer is currently "
   if global_options.send_help then
      mail_brief("Option -Z help for printer ",
		 option_help_message(config).. "\n"..status_msg..
		    status_inquire(config, timeouts))
      os.exit(lprng_exit_code.JSUCC);
   end

   if global_options.send_status then
      mail_info(status_msg..status_inquire(config, timeouts))
      os.exit(lprng_exit_code.JSUCC);
   end
end

local err = lpsend.initialize_io(
   table.join(timeouts,
	      { printer = config.device,
		write_limit = config.write_limit,
		strip_parity = global_options.strip_parity,
	      }))

if err then
   mail_alert(err == errno.EBUSY
		 and "Some other job is using the printer."
		 or "I failed to initialize the printer.  Is it plugged in?")
   error("Can't initialize io.  "..lpsend.errno_to_string(err))
end

if config.usbid then
   local matches, err = lpsend.check_vid_pid(config.usbid)
   if type(matches)=="nil" then
      error("Can't check VID/PID.  "..lpsend.errno_to_string(err))
   end
   if not matches then
      error("Printer doesn't match configuration")
   end
end

sendjob(config, timeouts, features, global_options.send_report)
