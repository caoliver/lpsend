local report, notices = {}
local report_length, suppressed = 0, 0
local max_notices, user, host, printer, id, time, send_report, suppress_mail
local complaints_to

function initialize_mail(options, config)
   user = options.user
   host = options.host
   printer = options.printer
   identifier = options.identifier
   time = options.time
   send_report = options.send_report
   suppress_mail = options.suppress_mail
   max_notices = options.max_notices
   complaints_to = config.complaints_to
end

function whine(condition)
   if (complaints_to) then
      lpsend.sendmail(complaints_to, string.format([[
To: %s
Priority: Urgent
Subject: LPSEND needs help

Printer %s %s.  Please fix ASAP!
.
]], complaints_to, printer, condition))
   end
end


function mail_brief(subject, message)
   if suppress_mail then return end
   local recipient = user..'@'..host
   lpsend.sendmail(recipient, string.format([[
To: %s
Subject: %s

%s.
]], recipient, subject..printer , message))
end

function mail_info(info)
   mail_brief("Information about printer ", info.."\n")
end

function mail_alert(alert)
   mail_brief("Alert condition for printer ", alert.."\n")
end

local function type_of_report()
   local function describe(number) return pluralize(number, "byte:s") end
   if suppressed + report_length == 0 then
      return "no readback."
   end
   if (report_length == 0) then
      return describe(suppressed).." of suppressed readback."
   end
   return "the following "..
	 describe(report_length).." of readback"..
      (suppressed == 0 and ":"
       or " with "..
       (suppressed + report_length > 10000 and "\n" or "")..
       describe(suppressed).." suppressed:")
end

function append_notice(message)
   if suppress_mail then return end
   if not notices then
      notices = { message }
   elseif #notices < (max_notices or 16) then
      table.insert(notices, message)
   end
end

function append_report(message)
   if suppress_mail or not send_report then return end
   if not report then
      report = { message }
   else
      table.insert(report, message)
   end
   report_length = #message + report_length
end

function suppress_report(bytes)
   suppressed = suppressed + bytes
end

function mail_report()
   if suppress_mail or not (notices or send_report) then return end

   local recipient = user..'@'..host
   local message = { string.format([[
Your job %s on printer %s submitted %s
produced ]], identifier, printer, time)}

   if (notices) then
      table.insert(message,
	     #notices > 1
		and "these notices:\n"
		or "this notice:\n")
      for _, notice in ipairs(notices) do
	 table.insert(message, "\n    ")
	 table.insert(message, notice)
      end
      if send_report then table.insert(message, "\n\nand produced ") end
   end

   if (send_report) then table.insert(message, type_of_report()) end

   if report and #report > 0 then
      table.insert(message, "\n\n")
      for _, report_line in ipairs(report) do
	 table.insert(message, report_line)
      end
      table.insert(message, "\n");
   end


   lpsend.sendmail(recipient, 
		   string.format([[
To: %s
Subject: PostScript output from print job_info on %s
Mime-Version: 1.0
Content-Type: text/plain; charset=us-ascii
Content-Transfer-Encoding: base64

%s]], recipient, printer, lpsend.base64_encode(table.concat(message), 18)))
end
