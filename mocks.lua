ffi = require "ffi"
ffi.cdef "void usleep(long)"
function sleep(msec) ffi.C.usleep(1000*msec) end

user_print_job = {
"%!PS\n",
"/in { 72 mul } def\n",
"/Times-Roman 1 in selectfont\n",
"2.5 in 4 in moveto\n",
"(Wombats) show showpage\n",
}

readback_data = {
   "barnacles and soap",
	 '@PJL USTATUS\r\nCODE=10023\r\nDISPLAY="PrintingDocument"\r\nONLINE=TRUE\r\n\f', 
	 "Aardvarks and avocados",
   "@PJL ECHO EOJ "..nonce.."\r\n\f",
	 "howdy, partner\r\n",

				      }

function stream_table(tbl, default)
   local generator, source, _ = pairs(tbl)
   local index, value = generator(source, nil)
   return function ()
      if index then
	 local return_value = value
	 index, value = generator(source, index)
	 return return_value
      else
	 return default
	 end
   end
end

do
   local readback
   local printer_read_stream = stream_table(readback_data, "")
   function mock_read_printer(len)
      if not readback then
	 return printer_read_stream()
      end
      local old = readback
      readback = nil
      return old
   end

   function select_readback()
      if readback then return true end
      local next = printer_read_stream()
      if #next == 0 then return false end
      readback = next
      return true
   end
end

input_stream = stream_table(user_print_job, "")

--  mock_read_stdin/printer(buffer_size) -> nil, error or string
function mock_read_stdin(bufsize)
   return input_stream()
end
--]]

--  mock_write_printer(string) -> false (nil), errno or bytes written
function mock_write_printer(str)
   io.write("PRINTER GETS: "..str)
   return #str
end
--]]


--  mock_select(bit.bor of check bits, timeout_usec or (nil))
--	      -> false (nil), errno or bit.bor of check bits
function mock_select(fds, timeout)
   local readback_available
   if select_readback() then
      if not stopwatch then
	 readback_available = true
	 stopwatch = lpsend.stopwatch()
      elseif stopwatch() >= 1 then
	 readback_available = true
	 stopwatch = nil
      end
   end
   local sel = bit.bor(bit.band(fds, lpsend.PRINT_WRITE),
		       bit.band(fds, lpsend.STDIN_READ),
		       (readback_available and lpsend.PRINT_READ or 0))
   if (sel == 0) then sleep(timeout / 1000) end
   return sel
end
--]]


--[[  mock_clock_gettime() -> tv_sec, tv_nsec
function mock_gettime()
end
--]]


--  mock_lpgetstatus() -> status(int) or nil, errno
function mock_lpgetstatus()
   return 24
end
--]]
