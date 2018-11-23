if os.getenv "DOFILE"  then
   function generate(progfile)
      print("dofile \""..progfile.."\"")
   end
else
   -- Combine lua files in a given order as do/end chunks.
   -- Add comments to indicate beginning and ending of chunks.
   function generate(progfile)
      local file,err=io.open(progfile, "r")
      if not file then error(err) end
      local comment = "  --[==[ "..progfile:upper().." ]==]\n\n"
      io.write("do ",comment,file:read("*a"),"\n\nend",comment,"\n")
      file:close()
   end
end

for _,progfile in ipairs(arg) do
   generate(progfile)
end
