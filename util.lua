function pluralize(count, unit, no_number)
   local _, _, stem, plural, singular = unit:find('([^:]*):([^:]*):?(.*)')
   local description = stem..(count == 1 and singular or plural)
   if (no_number) then return description end
   return (count == 0 and "no " or (tostring(count)..' '))..description
end

function table.join(...)
   local tabs, newtab = {...}, {}
   for _,t in ipairs(tabs) do
      for k,v in pairs(t) do newtab[k]=v end
   end
   return newtab
end

-- PJL readback scanners
status_pattern = '\r\nCODE=([0-9]+)\r\nDISPLAY=".*"\r\nONLINE=TRUE\r\n'
info_status_pattern = '^INFO STATUS'..status_pattern
ustatus_pattern = '^USTATUS DEVICE'..status_pattern
eoj_pattern = '^ECHO EOJ (.*)\r\n$'
