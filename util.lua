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
