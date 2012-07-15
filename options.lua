local specials = {
   help = {"send_help",
	   "Send this help by mail."};
   status = {"send_status",
	     "Mail back if the printer is powered up or not."}
}

function unique(tbl)
   local list =  {}
   for val, _ in pairs(tbl) do table.insert(list, val) end
   table.sort(list)
   return list
end

function option_help_message(config)
   local function jabber_message(limit)
      if (limit == 0) then return "Disabled.  This option is ignored." end
      local jabber_message = "Mail PostScript output to submitter"
      if not limit then return jabber_message.."." end
      local units = { "char", "KB", "MB" }
      local unit, denom = 1, 1
      while unit < 3 and limit/denom >= 1024 do
	 unit, denom = unit + 1, denom * 1024
      end
      return string.format(
	 (limit % denom == 0 and "%s up to %d %s." or "%s up to %.2f %s."),
	 jabber_message, 
	 limit/denom,
	 pluralize(limit/denom, units[unit]..":s", true))
   end

   local function specials_message()
      local lines = {}
      for key, special in pairs(specials) do
	 table.insert(lines, key..string.rep(" ", 11 - #key)..special[2])
      end
      return "  "..table.concat(lines, "\n  ")
   end

   return string.format(
   [[
Usage: lpr -Z [global_options][/feature_codes] ...

global options:

%s
  m          %s
  M          Suppress any report by mail.
  p          Clear parity bit on input data.

%s]],
   specials_message(),
   jabber_message(config.jabber_limit),
   config.documentation)
end

function process_options(arg, config)
   local option_value={}

   local option_to_name = {
      n = 'user',
      h = 'host',
      j = 'identifier',
      t = 'time',
      P = 'printer',
   }

   local printer_features, has_features, global_options = '', '', ''
   for i,option_string in ipairs(arg) do
      local found,_,option,value = option_string:find('^-(.)(.*)$')
      if not found then break end
      if option_to_name[option] then
	 option_value[option_to_name[option]] = value
      elseif option == 'Z' then
	 _, _, global_options, has_features, printer_features =
	    value:find("^([^/]*)(/?)([^/]*).*$")
      end
   end

   -- Interepret global_options field as a command
   if has_features == '' then
      for key, special in pairs(specials) do
	 if global_options == key then
	    option_value[special[1]] = true
	    return true, option_value
	 end
      end
   end

   local invalid_global_options, invalid_features = {}, {}
   local features=config.postscript.defaults, invalid_features
   for i = 1, #printer_features do
      local feature = printer_features:sub(i,i)
      local masks = config.postscript.switches[feature]
      if not masks then
	 invalid_features[feature] = true
      else
	 features = bit.bor(bit.band(features, masks[1]), masks[2])
      end
   end
   invalid_features = unique(invalid_features)

   option_value.strip_parity = false
   option_value.send_report = false
   option_value.suppress_mail = false
   for i = 1, #global_options do
      local glob_opt = global_options:sub(i,i)
      if glob_opt == "p" then
	 option_value.strip_parity = true
      elseif glob_opt == "M" then
	 option_value.send_report = false
	 option_value.suppress_mail = true
      elseif glob_opt == "m" then
	 option_value.suppress_mail = false
	 option_value.send_report = config.jabber ~= 0
      else
	 invalid_global_options[glob_opt] = true
      end
   end
   invalid_global_options = unique(invalid_global_options)

   if #invalid_global_options == 0 and #invalid_features == 0 then
      return features, option_value
   end
   local message = "Invalid -Z switch\n\n"
   if #invalid_global_options > 0 then
      message = message.."  "..
	 pluralize(#invalid_global_options, "invalid global option:s")..": "..
	 table.concat(invalid_global_options, ", ")
   end
   if #message > 0 and #invalid_global_options > 0 then
      message = message.."\n"
   end
   if #invalid_features > 0 then
      message = message.."  "..
	 pluralize(#invalid_features, "invalid feature:s")..": "..
	 table.concat(invalid_features, ", ")
   end
   return nil, option_value, message
end
