-- Mockup for PostScript error recognizer

function with_catchall(default, transition_list)
   setmetatable(transition_list, {__index=function () return default end})
   return transition_list
end

local dfa_transition_table = {
   q0 = with_catchall('q0', { ['%'] = 'q1', [' '] = 'q17' }),
   q1 = with_catchall('q0', { ['%'] = 'q2', [' '] = 'q17' }),
   q2 = with_catchall('q0', { ['%'] = 'q2', ['['] = 'q3', [' '] = 'q17' }),
   q3 = with_catchall('q0', { ['%'] = 'q1', [' '] = 'q4' }),
   q4 = with_catchall('q4', { [' '] = 'q5', ['%'] = 'q11', [']'] = 'q22' }),
   q5 = with_catchall('q4', { [']'] = 'q6', ['%'] = 'q11' }),
   q6 = with_catchall('q4', { [' '] = 'q5', ['%'] = 'q7' }),
   q7 = with_catchall('q4', { [' '] = 'q5', ['%'] = 'q8' }),
   q8 = with_catchall('q4', { [' '] = 'q5', ['\r'] = 'q9' }),
   q9 = with_catchall('q4', { [' '] = 'q5', ['\n'] = 'q10' }),
   q10 = with_catchall('q0', { }),
   q11 = with_catchall('q4', { [' '] = 'q5', ['%'] = 'q12' }),
   q12 = with_catchall('q4', { [' '] = 'q5', ['['] = 'q13' }),
   q13 = with_catchall('q4', { [' '] = 'q16', ['%'] = 'q11' }),
   q14 = with_catchall('q0', { ['%'] = 'q15' }),
   q15 = with_catchall('q0', { ['%'] = 'q16' }),
   q16 = with_catchall('q16', { }),
   q17 = with_catchall('q0' , { ['%'] = 'q1', [']'] = 'q18' }),
   q18 = with_catchall('q0' , { ['%'] = 'q19', [' '] = 'q18' }),
   q19 = with_catchall('q0' , { ['%'] = 'q20', [' '] = 'q18' }),
   q20 = with_catchall('q0' , { [ '\r' ] = 'q21', [' '] = 'q18' }),
   q21 = with_catchall('q0' , { [ '\n' ] = 'q16', [' '] = 'q18' }),
   q22 = with_catchall('q4' , { ['%'] = 'q23', [' '] = 'q5' }),
   q23 = with_catchall('q4' , { ['%'] = 'q24', [' '] = 'q5'}),
   q24 = with_catchall('q4' , { [ '\r' ] = 'q25', [' '] = 'q5' }),
   q25 = with_catchall('q4' , { [ '\n' ] = 'q16', [' '] = 'q5' }),
}

local nicknames = {
   q0 = "Q_START",
   q1 = "Q_PERCENT",
   q2 = "Q_PERCENT_AGAIN",
   q10 = "Q_PS_MSG",
   q16 = "Q_STRAY_DELIM"
}

function scan(str, state, offset)
   local beginning = 0
   local action = {
      Q_START = function () beginning = 0 end,
      Q_PERCENT = function (i) beginning = i end,
      Q_PERCENT_AGAIN = function (i) beginning = i - 1 end,
      Q_PS_MSG = function (i)
	 return string.format('prefix: "%s"\nstate: %s\nfound: %s',
			      str:sub(1, beginning - 1),
			      state,
			      str:sub(beginning,i))
      end,
      Q_STRAY_DELIM = function (i)
	 return string.format('Stray delimiter at %d', i)
      end
   }
   setmetatable(action, { __index=function () return function () end end })
   state = state or 'q0'
   offset = offset or 0
   for i = offset + 1, #str do
      state = dfa_transition_table[state][str:sub(i,i)]
      local result = action[nicknames[state]](i)
      if result then return result end
   end
   return string.format('prefix: "%s"\nstate: %s\n', 
			str:sub(1, beginning - 1),
			state)
end

C_translations = { 
   ['\n'] = '\\n',
   ['\r'] = '\\r',
}

do
   local chtab = {}
   local revchtab = {}
   local chix = 1;
   local revsttab = {}
   local stix = 1;
   local sttab = {}

   function initialize_tables()
      for state_name, _ in pairs(dfa_transition_table) do
	 table.insert(sttab, state_name)
      end
      for ix, state_name in ipairs(sttab) do
	 local state = dfa_transition_table[state_name]

	 revsttab[state_name] = ix

	 stix = stix + 1
	 for char in pairs(state) do
	    if chtab[char] == nil then
	       chtab[char] = chix
	       revchtab[chix] = char
	       chix = chix + 1
	    end
	 end
      end
   end

   function write_byte_class_table(array_declaration)
      print(array_declaration.."[256] = {")
      for char,name in ipairs(revchtab) do
	 print(string.format("  ['%s'] = %d,",
			     (C_translations[name] or name),
			     char))
      end
      print "};\n"
   end

   function write_transition_table(array_declaration)
      print(string.format(array_declaration.."[][%d] = {", chix))
      for i, state_name in ipairs(sttab) do
	 local outline = "  { "
	 local state = dfa_transition_table[state_name]

	 outline = outline..string.format("%d",
					  revsttab[state["NOMATCH"]] - 1)
	 for ch = 1, chix-1 do
	    outline = outline..string.format(", %d",
					     revsttab[state[revchtab[ch]]] - 1)
	 end
	 print(string.format("%s },%s",
			     outline,
			     nicknames[state_name] and
				string.rep(" ", 40 - #outline)..'// '..
				nicknames[state_name] or
				''))
      end
      print "};\n"
   end

   function write_nickname_defines(prefix)
      for state_name, nickname in pairs(nicknames) do
	 print(string.format("#define %s%s %d",
			     prefix,
			     nickname,
			     revsttab[state_name] - 1))
      end
   end
end

dot_translations = { 
   ['\n'] = 'newline',
   ['\r'] = 'return',
   [' '] = 'space',
}

function visualize(start)
   print [[
digraph g {
page="8.5,11.0"
size="7,10"
center=1
"" [ shape=none ]
]]
   for state_name, nickname in pairs(nicknames) do
      print(string.format('%s [ label="\\N\\n%s" peripheries=2 ]', state_name, nickname))
   end
   print ('"" -> '..start)
   for state,gotofn in pairs(dfa_transition_table) do
      print (state..' -> '..gotofn.NOMATCH..' [ label = "else" ]')
      for symbol,next_state in pairs(gotofn) do
	 print (state..' -> '..next_state.. ' [ label = "'..
		(dot_translations[symbol] or symbol)..'" ]')
      end
   end
   print "}"
end

if arg[1] == "visualize" then
   visualize('q0')
elseif arg[1] == "generate" then
   initialize_tables()
   write_byte_class_table(arg[2])
   write_transition_table(arg[3])
   write_nickname_defines(arg[4])
end
