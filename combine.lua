-- Combine lua files in a given order as do/end chunks.
-- Add comments to indicate beginning and ending of chunks.

if os.getenv "DOFILE"  then
   function generate(progfile)
      print("dofile \""..progfile.."\"")
   end
else
   function generate(progfile)
      local file,err=io.open(progfile, "r")
      if not file then error(err) end
      io.write(table.concat {
		  "do   --[==[ ",
		  progfile:upper(),
		  " ]==]\n\n",
		  file:read("*a"),
		  "\n\nend  --[==[ ",
		  progfile:upper(),
		  " ]==]\n\n\n"
			    })
      file:close()
   end
end

for _,progfile in ipairs(arg) do
   generate(progfile)
end
