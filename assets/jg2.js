/* gitws.js: javascript functions for gitws
 *
 * Copyright (C) 2018 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 */

/* http://i18njs.com/ this from http://i18njs.com/js/i18n.js */
(function() {
  var Translator, i18n, translator,
    __bind = function(fn, me){ return function(){ return fn.apply(me, arguments); }; };

  Translator = (function() {
    function Translator() {
      this.translate = __bind(this.translate, this);      this.data = {
        values: {},
        contexts: []
      };
      this.globalContext = {};
    }

    Translator.prototype.translate = function(text, defaultNumOrFormatting, numOrFormattingOrContext, formattingOrContext, context) {
      var defaultText, formatting, isObject, num;

      if (context == null) {
        context = this.globalContext;
      }
      isObject = function(obj) {
        var type;

        type = typeof obj;
        return type === "function" || type === "object" && !!obj;
      };
      if (isObject(defaultNumOrFormatting)) {
        defaultText = null;
        num = null;
        formatting = defaultNumOrFormatting;
        context = numOrFormattingOrContext || this.globalContext;
      } else {
        if (typeof defaultNumOrFormatting === "number") {
          defaultText = null;
          num = defaultNumOrFormatting;
          formatting = numOrFormattingOrContext;
          context = formattingOrContext || this.globalContext;
        } else {
          defaultText = defaultNumOrFormatting;
          if (typeof numOrFormattingOrContext === "number") {
            num = numOrFormattingOrContext;
            formatting = formattingOrContext;
            context = context;
          } else {
            num = null;
            formatting = numOrFormattingOrContext;
            context = formattingOrContext || this.globalContext;
          }
        }
      }
      if (isObject(text)) {
        if (isObject(text['i18n'])) {
          text = text['i18n'];
        }
        return this.translateHash(text, context);
      } else {
        return this.translateText(text, num, formatting, context, defaultText);
      }
    };

    Translator.prototype.add = function(d) {
      var c, v, _i, _len, _ref, _ref1, _results;

      if ((d.values != null)) {
        _ref = d.values;
        var k;
        for (k in _ref) {
      	  if ({}.hasOwnProperty.call(_ref, k)) {
          v = _ref[k];
          this.data.values[k] = v;
      	  }
        }
      }
      if ((d.contexts != null)) {
        _ref1 = d.contexts;
        _results = [];
        for (_i = 0, _len = _ref1.length; _i < _len; _i++) {
          c = _ref1[_i];
          _results.push(this.data.contexts.push(c));
        }
        return _results;
      }
    };

    Translator.prototype.setContext = function(key, value) {
      return this.globalContext[key] = value;
    };

    Translator.prototype.clearContext = function(key) {
      return this.lobalContext[key] = null;
    };

    Translator.prototype.reset = function() {
      this.data = {
        values: {},
        contexts: []
      };
      return this.globalContext = {};
    };

    Translator.prototype.resetData = function() {
      return this.data = {
        values: {},
        contexts: []
      };
    };

    Translator.prototype.resetContext = function() {
      return this.globalContext = {};
    };

    Translator.prototype.translateHash = function(hash, context) {
      var k, v;

      for (k in hash) {
    	  if ({}.hasOwnProperty.call(hash, k)) {
	        v = hash[k];
	        if (typeof v === "string") {
	          hash[k] = this.translateText(v, null, null, context);
	        }
    	  }
      }
      return hash;
    };

    Translator.prototype.translateText = function(text, num, formatting, context, defaultText) {
      var contextData, result;

      if (context == null) {
        context = this.globalContext;
      }
      if (this.data == null) {
        return this.useOriginalText(defaultText || text, num, formatting);
      }
      contextData = this.getContextData(this.data, context);
      if (contextData != null) {
        result = this.findTranslation(text, num, formatting, contextData.values, defaultText);
      }
      if (result == null) {
        result = this.findTranslation(text, num, formatting, this.data.values, defaultText);
      }
      if (result == null) {
        return this.useOriginalText(defaultText || text, num, formatting);
      }
      return result;
    };

    Translator.prototype.findTranslation = function(text, num, formatting, data) {
      var result, triple, value, _i, _len;

      value = data[text];
      if (value == null) {
        return null;
      }
      if (num == null) {
        if (typeof value === "string") {
          return this.applyFormatting(value, num, formatting);
        }
      } else {
        if (value instanceof Array || value.length) {
          for (_i = 0, _len = value.length; _i < _len; _i++) {
            triple = value[_i];
            if ((num >= triple[0] || triple[0] === null) && (num <= triple[1] || triple[1] === null)) {
              result = this.applyFormatting(triple[2].replace("-%n", String(-num)), num, formatting);
              return this.applyFormatting(result.replace("%n", String(num)), num, formatting);
            }
          }
        }
      }
      return null;
    };

    Translator.prototype.getContextData = function(data, context) {
      var c, equal, key, value, _i, _len, _ref, _ref1;

      if (data.contexts == null) {
        return null;
      }
      _ref = data.contexts;
      for (_i = 0, _len = _ref.length; _i < _len; _i++) {
        c = _ref[_i];
        equal = true;
        _ref1 = c.matches;
        for (key in _ref1) {
        	if ({}.hasOwnProperty.call(_ref1, key)) {
        		value = _ref1[key];
        		equal = equal && value === context[key];
        	}
        }
        if (equal) {
          return c;
        }
      }
      return null;
    };

    Translator.prototype.useOriginalText = function(text, num, formatting) {
      if (num == null) {
        return this.applyFormatting(text, num, formatting);
      }
      return this.applyFormatting(text.replace("%n", String(num)), num, formatting);
    };

    Translator.prototype.applyFormatting = function(text, num, formatting) {
      var ind, regex;

      for (ind in formatting) {
    	  if ({}.hasOwnProperty.call(formatting, ind)) {
	        regex = new RegExp("%{" + ind + "}", "g");
	        text = text.replace(regex, formatting[ind]);
    	  }
      }
      return text;
    };

    return Translator;

  })();

  translator = new Translator();

  i18n = translator.translate;

  i18n.translator = translator;

  i18n.create = function(data) {
    var trans;

    trans = new Translator();
    if (data != null) {
      trans.add(data);
    }
    trans.translate.create = i18n.create;
    return trans.translate;
  };

  (typeof module !== "undefined" && module !== null ? module.exports = i18n : void 0) || 
  	(this.i18n = i18n);

}.call(this));

var lang_ja = "{" +
  "\"values\":{" +
    "\"Summary\": \"概要\"," +
    "\"Log\": \"ログ\"," +
    "\"Tree\": \"木構造\"," +
    "\"Blame\": \"責任\"," +
    "\"Copy Lines\": \"コピーライン\"," +
    "\"Copy Link\": \"リンクをコピーする\"," +
    "\"View Blame\": \"責任がある\"," +
    "\"Remove Blame\": \"責任を取り除く\"," +
    "\"Mode\": \"モード\"," +
    "\"Size\": \"サイズ\"," +
    "\"Name\": \"名\"," +
    "\"s\": \"秒\"," +
    "\"m\": \"分\"," +
    "\"h\": \"時間\"," +
    "\" days\": \"日々\"," +
	"\" weeks\": \"週\"," +
	"\" months\": \"数ヶ月\"," +
	"\" years\": \"年\"," +
	"\"Branch Snapshot\": \"ブランチスナップショット\"," +
	"\"Tag Snapshot\": \"タグスナップショット\"," +
	"\"Commit Snapshot\": \"スナップショットをコミットする\"," +
	"\"Description\": \"説明\"," +
	"\"Owner\": \"オーナー\"," +
	"\"Branch\": \"ブランチ\"," +
	"\"Tag\": \"タグ\"," +
	"\"Author\": \"著者\"," +
	"\"Age\": \"年齢\"," +
	"\"Page fetched\": \"ページを取得した\"," +
	"\"creation time\": \"作成時間\"," +
	"\"created\": \"作成した\"," +
	"\"ago\": \"前\"," +
	"\"Message\": \"メッセージ\"," +
	"\"Download\": \"ダウンロード\"," +
	"\"root\": \"ルート\"," +
	"\"Committer\": \"コミッター\"," +
	"\"Raw Patch\": \"生パッチ\"," +
	"\"Page fetched %{pf} ago, creation time: %{ct}ms " +
 	   "(vhost etag hits: %{ve}%, cache hits: %{ch}%)\": " +
 	"\"%{pf}間前に取得されたページ, 作成にかかった時間: %{ct}ms " +
 	   "(vhost etag キャッシュヒット: %{ve}%, キャッシュヒット: %{ch}%)\"," +
 	"\"Created %{pf} ago, creation time: %{ct}ms \":" +
 	   "\"%{pf}間前に作成されました, 作成にかかった時間: %{ct}ms\"" +
  "}}";

var lang_zht = "{" +
"\"values\":{" +
  "\"Summary\": \"概要\"," +
  "\"Log\": \"日誌\"," +
  "\"Tree\": \"樹\"," +
  "\"Blame\": \"責怪\"," +
  "\"Copy Lines\": \"複製線\"," +
  "\"Copy Link\": \"複製鏈接\"," +
  "\"View Blame\": \"看責怪\"," +
  "\"Remove Blame\": \"刪除責怪\"," +
  "\"Mode\": \"模式\"," +
  "\"Size\": \"尺寸\"," +
  "\"Name\": \"名稱\"," +
  "\"s\": \"秒\"," +
  "\"m\": \"分鐘\"," +
  "\"h\": \"小時\"," +
  "\" days\": \"天\"," +
  "\" weeks\": \"週\"," +
  "\" months\": \"個月\"," +
  "\" years\": \"年份\"," +
  "\"Branch Snapshot\": \"科快照\"," +
  "\"Tag Snapshot\": \"标签快照\"," +
  "\"Commit Snapshot\": \"提交快照\"," +
  "\"Description\": \"描述\"," +
  "\"Owner\": \"所有者\"," +
  "\"Branch\": \"科\"," +
  "\"Tag\": \"標籤\"," +
  "\"Author\": \"作者\"," +
  "\"Age\": \"年齡\"," +
  "\"Page fetched\": \"頁面已獲取\"," +
  "\"creation time\": \"創作時間\"," +
  "\"created\": \"創建\"," +
  "\"ago\": \"前\"," +
  "\"Message\": \"信息\"," +
  "\"Download\": \"下載\"," +
  "\"root\": \"根源\"," +
  "\"Committer\": \"提交者\"," +
  "\"Raw Patch\": \"原始補丁\"," +
  "\"Page fetched %{pf} ago, creation time: %{ct}ms " +
  	   "(vhost etag hits: %{ve}%, cache hits: %{ch}%)\": " +
  	"\"頁面%{pf}前獲取, 創作時間: %{ct}ms " +
  	   "(vhost etag 緩存命中: %{ve}%, 緩存命中: %{ch}%)\"," +
  "\"Created %{pf} ago, creation time: %{ct}ms \":" +
  	"\"%{pf}前創建, 創作時間: %{ct}ms \"" +
"}}";

var lang_zhs = "{" +
"\"values\":{" +
  "\"Summary\": \"概要\"," +
  "\"Log\": \"日志\"," +
  "\"Tree\": \"木\"," +
  "\"Blame\": \"归咎\"," +
  "\"Copy Lines\": \"复制线\"," +
  "\"Copy Link\": \"复制链接\"," +
  "\"View Blame\": \"看责备\"," +
  "\"Remove Blame\": \"删除责备\"," +
  "\"Mode\": \"模式\"," +
  "\"Size\": \"尺寸\"," +
  "\"Name\": \"名称\"," +
  "\"s\": \"秒\"," +
  "\"m\": \"分钟\"," +
  "\"h\": \"小时\"," +
  "\" days\": \"天\"," +
  "\" weeks\": \"周\"," +
  "\" months\": \"个月\"," +
  "\" years\": \"年份\"," +
  "\"Branch Snapshot\": \"科快照\"," +
  "\"Tag Snapshot\": \"标签快照\"," +
  "\"Commit Snapshot\": \"提交快照\"," +
  "\"Description\": \"描述\"," +
  "\"Owner\": \"所有者\"," +
  "\"Branch\": \"科\"," +
  "\"Tag\": \"标签\"," +
  "\"Author\": \"作者\"," +
  "\"Age\": \"年龄\"," +
  "\"Page fetched\": \"页面已获取\"," +
  "\"creation time\": \"创作时间\"," +
  "\"created\": \"创建\"," +
  "\"ago\": \"前\"," +
  "\"Message\": \"信息\"," +
  "\"Download\": \"下载\"," +
  "\"root\": \"根源\"," +
  "\"Committer\": \"提交者\"," +
  "\"Raw Patch\": \"原始补丁\"," +
  "\"Page fetched %{pf} ago, creation time: %{ct}ms " +
	   "(vhost etag hits: %{ve}%, cache hits: %{ch}%)\": " +
	"\"页面%{pf}前获取, 创作时间: %{ct}ms " +
	   "(vhost etag 缓存命中: %{ve}%, 缓存命中: %{ch}%)\"," +
   "\"Created %{pf} ago, creation time: %{ct}ms \":" +
	  	"\"%{pf}前创建, 创作时间: %{ct}ms \"" +	   
"}}";


(function () {
	
function san(s)
{
	if (!s)
		return "";

	return s.replace(/&/g, "&amp;").
		     replace(/\</g, "&lt;").
		     replace(/\>/g, "&gt;").
			 replace(/\"/g, "&quot;").
			 replace(/%/g, "&#37;");
}

function san_nq(s)
{
	if (!s)
		return "";

	return s.replace(/&/g, "&amp;").
		     replace(/</g, "&lt;&#8203;").
		     replace(/>/g, "&gt;").
			 replace(/%/g, "&#37;");
}

var burger, menu_popup, j_avatar, menu_popup_archive, jf, blog_mode = 0;

function collect_offsetTop(e1)
{
	var t = 0;

	while (e1) {
		if (e1.offsetTop)
			t += e1.offsetTop;
		e1 = e1.offsetParent;
	}

	return t;
}

function collect_offsetLeft(e1)
{
	var t = 0;

	while (e1) {
		if (e1.offsetLeft)
			t += e1.offsetLeft;
		e1 = e1.offsetParent;
	}

	return t;
}

function find_parent_of_type(e, type)
{
	while (e.tagName.toLowerCase() !== type)
		e = e.parentNode;

	return e;
}

function parse_hashurl_start(h)
{
	return parseInt(h.substring(2), 10);
}

function parse_hashurl_end(h, start)
{
	var t = h.indexOf("-"), e = start;

	if (t >= 1)
		e = parseInt(h.substring(t + 1), 10);

	if (e < start)
		e = start;

	return e;
}

function burger_create(e)
{
	var e1 = e, etable, d = new Date;

	if (burger)
		burger.remove();

	burger = document.createElement("DIV");
	burger.className = "burger";
	burger.style.top = collect_offsetTop(e1) + "px";

	/* event listener cannot override default browser #URL behaviour */
	burger.onclick = burger_click;

	etable = find_parent_of_type(e, "table");
	etable.insertBefore(burger, etable.firstChild);
	burger_time = d.getTime();

	setTimeout(function() {
		burger.style.opacity = "1";
	}, 1);
}

/*
 * This creates an absolute div as a child of the content table.
 * It's horizontally and vertically aligned and sized according
 * to the #URL information like #n123-456
 * 
 * If the highlight div already exists, it's removed and remade.
 */

function line_range_highlight(do_burger)
{
	var h = window.location.hash, l1 = 0, l2 = 0, e, t;

	e = document.getElementById("sel-line-range");
	if (e) {
		l1 = e.l1;

		while (l1 <= e.l2) {
			var e1;
			e1 = document.getElementById('n' + l1++);
				e1.classList.remove(
					'selected-line-link-highlight');
		}

		e.remove();
	}

	l1 = parse_hashurl_start(h);
	if (!l1)
		return;

	l2 = parse_hashurl_end(h, l1);

	var lh, etable, etr, de, n, hl, v;

	e = document.getElementById('n' + l1);
	if (!e)
		return;

	if (do_burger)
		burger_create(e);

	de = document.createElement("DIV");

	de.className = "selected-lines";
	de.style.bottom = e.style.bottom;
	de.style.top = collect_offsetTop(e) + 'px';
	de.id = "sel-line-range";
	de.l1 = l1;
	de.l2 = l2;

	/* we will tack the highlight div at the parent tr */
	etr = find_parent_of_type(e, "tbody");

	de.style.width = etr.offsetWidth + 'px';

	/* the table is offset from the left, the highlight needs to follow it */
	etable = find_parent_of_type(etr, "table");
	//de.style.left = etable.offsetLeft + 'px';
	de.style.height = ((l2 - l1 + 1) * e.offsetHeight) + 'px';

	etr.insertBefore(de, etr.firstChild);

	setTimeout(function() {
		de.style.backgroundColor = "rgba(255, 255, 0, 0.2)";
	}, 1);

	n = l1;
	while (n <= l2)
		document.getElementById('n' + n++).classList.add(
					'selected-line-link-highlight');

	hl = (window.innerHeight / (e.offsetHeight + 1));
	v = (l1 + ((l2 - l1) / 2)) - (hl / 2);
	if (v > l1)
		v = l1;
	if (v < 1)
		v = 1;

	t = document.getElementById('n' + Math.round(v));
	if (!t)
		t = e;

	t.scrollIntoView(true);
}

function copy_clipboard(value, e)
{
	var inp = document.createElement("textarea");

	inp.type = "text";
	inp.value = value;
	/* hidden style stops it working for clipboard */
	inp.setAttribute('readonly', '');
	inp.style.position = "absolute";
	inp.style.left = "-1000px";

	e.appendChild(inp);

	inp.select();

	document.execCommand("copy");

	inp.remove();
}

/* we want to copy plain text for a line range */

function copy_text(elem, l1, l2)
{
	var tc = elem.textContent.split('\n'), s = "";

	l1 --;

	while (l1 < l2)
		s += tc[l1++] + '\n';

	copy_clipboard(s, document.getElementById("jglinenumbers"));
}


var branches = new(Array), tags = new(Array), relpre_no_mode, relpost,
				vpath, reponame, rmode, rpath, qbranch, qid, qofs, qsearch;

function makeurl(_reponame, _mode, _rpath, _qbranch, _qid, _qofs, _qs)
{
	var base = window.location.pathname, c = '?', b = vpath;

	if (_reponame && b[0] !== '/')
		b += '/';
	if (_reponame)
		b += _reponame;
	if (_mode)
		b += '/' + _mode;
	if (_rpath && _rpath != null)
		b += '/' + _rpath;
	
	if (_qbranch) {
		b += '?h=' + _qbranch;
		c = '&';
	}
	
	if (_qid) {
		b += c + 'id=' + _qid;
		c = '&';
	}
	
	if (_qofs) {
		b += c + 'ofs=' + _qofs;
		c = '&';
	}
	
	if (_qs) {
		b += c + 'q=' + _qs;
		c = '&';
	}

	return san(b);
}

/*
 * An element in the popup menu was clicked, perform the appropriate action
 */
function mi_click(e) {
	var u, n, l1, l2, el;

	e.stopPropagation();
	e.preventDefault();
	
	u = "";
	if (window.location.hash.length)
		u = window.location.hash;

	switch (e.target.id) {
	case "mi-c-line":
		l1 = parse_hashurl_start(window.location.hash);
		l2 = parse_hashurl_end(window.location.hash, l1);
		el = document.getElementsByClassName("doc")[0].firstChild.firstChild;
		copy_text(el, l1, l2);
		break;
	case "mi-c-link":
		copy_clipboard(window.location.href, document.getElementById("jglinenumbers"));
		break;
	case "mi-c-blame":
		window.location = makeurl(reponame, "blame", rpath, qbranch, qid, qofs) + u;
		break;
	case "mi-c-tree":
		window.location = makeurl(reponame, "tree", rpath, qbranch, qid, qofs) + u;
		break;
	}

	if (!menu_popup)
		return;

	menu_popup.remove();
	menu_popup = null;
}

/* We got a click on the (***) burger menu */

function burger_click(e) {
	var e1 = e, etable, d = new Date, s = "", n, is_blame,
	    ar = new Array("mi-c-line", "mi-c-link", "mi-c-blame", "mi-c-tree"),
	    an = new Array(i18n("Copy Lines"), i18n("Copy Link"),
	    		i18n("View Blame"), /* 2: shown in /tree/ */
	    		i18n("Remove Blame") /* 3: shown in /blame/ */);

	e.preventDefault();

	if (menu_popup) {
		menu_popup.remove();
		menu_popup = null;

		return;
	}

	/*
	 * Create the popup menu
	 */

	is_blame = !!rmode && rmode === "blame";

	menu_popup = document.createElement("DIV");
	menu_popup.className = "popup-menu";
	menu_popup.style.top = collect_offsetTop(e1) + e.offsetHeight + "px";

	s = "<ul id='menu-ul'>";
	for (n = 0; n < an.length; n++)
		if (n < 2 || is_blame === (n === 3))
			s += "<li id='" + ar[n] + "' tabindex='" + n + "'>" +
				an[n] + "</li>";
		    
	menu_popup.innerHTML = s;

	burger.insertBefore(menu_popup, null);

        document.getElementById(ar[0]).focus();
	for (n = 0; n < an.length; n++)
		if (n < 2 || is_blame === (n === 3))
			document.getElementById(ar[n]).
				addEventListener("click", mi_click);
				
	setTimeout(function() {
		menu_popup.style.opacity = "1";
	}, 1);

	/* detect loss of focus for popup menu */
	menu_popup.addEventListener("focusout", function(e) {
		/* if focus went to a child (menu item), ignore */
		if (e.relatedTarget &&
		    e.relatedTarget.parentNode.id === "menu-ul")
			return;

		menu_popup.remove();
		menu_popup = null;
	});
}

/*
 * We got a click on a line number #url
 *
 * Create the "burger" menu there.
 *
 * Redraw the line range highlight accordingly.
 */

function line_range_click(e) {
	var t, elem, m, n = window.location.href.length -
			    window.location.hash.length;

	/* disable passthru to stop scrolling by browser #URL handler */
	e.stopPropagation();
	e.preventDefault();

	if (!e.target.id)
		return;

	if (menu_popup) {
		menu_popup.remove();
		menu_popup = null;

		return;
	}
	
	if (e.target.id === "jglinenumbers")
		return;

	elem = document.getElementById(e.target.id);
	if (!elem)
		return;

	burger_create(elem);

	if (!window.location.hash ||
	    window.location.hash.indexOf("-") >= 0 ||
	    e.target.id.substring(1) === window.location.href.substring(n + 2))
		t = window.location.href.substring(0, n) +
		    '#n' + e.target.id.substring(1);
	else {
		if (parseInt(window.location.hash.substring(2), 10) <
		    parseInt(e.target.id.substring(1), 10))  /* forwards */
			t = window.location + '-' + e.target.id.substring(1);
		else
			t = window.location.href.substring(0, n) +
				'#n' + e.target.id.substring(1) + '-' +
				window.location.href.substring(n + 2);
	}

	window.history.replaceState(null, null, t);

	line_range_highlight(0);
}

function get_appropriate_ws_url(extra_url)
{
	var pcol;
	var u = document.URL;

	/*
	 * We open the websocket encrypted if this page came on an
	 * https:// url itself, otherwise unencrypted
	 */

	if (u.substring(0, 5) === "https") {
		pcol = "wss://";
		u = u.substr(8);
	} else {
		pcol = "ws://";
		if (u.substring(0, 4) === "http")
			u = u.substr(7);
	}

	u = u.split('/');

	/* + "/xxx" bit is for IE10 workaround */

	return pcol + u[0] + "/" + extra_url;
}

var age_names = [  "s",  "m",    "h", " days", " weeks", " months", " years" ];
var age_div =   [   1,   60,   3600,   86400,   604800,   2419200,  31536000  ];
var age_limit = [ 120, 7200, 172800, 1209600,  4838400,  63072000,         0  ];
var age_upd   = [   5,   10,    300,    1800,     3600, 12 * 3600, 12 * 3600  ];

function agify(now, secs)
{
	var d = now - secs, n;
	
	if (!secs)
		return "";
	
	for (n = 0; n < age_names.length; n++)
		if (d < age_limit[n] || age_limit[n] === 0)
			return "<span class='age-" + n + "' ut='" + secs +
				"'>" + Math.ceil(d / age_div[n]) +
				i18n(age_names[n]) + "</span>";
}

function aging()
{
        var n, next = 24 * 3600,
            now_ut = Math.round((new Date().getTime() / 1000));

        for (n = 0; n < age_names.length; n++) {
                var m, elems = document.getElementsByClassName(
                				"age-" + n), ne = elems.length,
                				a = new Array(), ee = new Array();

                if (elems.length && age_upd[n] < next)
                        next = age_upd[n];

                for (m = 0; m < ne; m++) {
                        var e = elems[m], secs = elems[m].getAttribute("ut");

                        a.push(agify(now_ut, secs));
                        ee.push(e);
                }
                
                for (m = 0; m < ee.length; m++) {
                	ee[m].innerHTML = a[m]; 
                }
        }

        /*
         * We only need to come back when the age might have changed.
         * Eg, if everything is counted in hours already, once per
         * 5 minutes is accurate enough.
         */

        window.setTimeout(aging, next * 1000);
}


function comp_reftime(a, b)
{
	return b.summary.time - a.summary.time;
}

function comp_reftime_tag(a, b)
{
	return b.summary.time - a.summary.time;
}

var loca;

function tz_date(ut, z)
{
	var d = new Date((ut) * 1000);
	
	if (loca) {
		var options = { weekday: 'long', year: 'numeric', month: 'long',
						day: 'numeric', hour: 'numeric', minute: 'numeric',
						second: 'numeric'};

		return d.toLocaleDateString(loca, options);
	}
	
	return d;
}

function identity_av_base(i)
{
	var em;
	
	if (j_avatar !== "//www.gravatar.com/avatar/") {
		em = j_avatar + i.md5 + "_avatar";
	} else
		em = j_avatar + i.md5 + "?s=128&amp;d=retro";
	
	return em;
}

function identity_img(i, size)
{
	var v = identity_av_base(i);
	
	return "<img class=\"inline-identity\" src=\"" + v + "\" alt=\"[]\" />";
}

function identity(i, size, parts)
{
	var s = "", em;

	if (!i || !i.email || !i.md5)
		return "";

	if (!!parts)
		s = "<span class='identity'>";
	
	s += "<a class='grav-mailto' href='mailto:" + san(i.email) + "'>" +
	
		"<span class=gravatar" + size + " alt='" + san(i.email) +
		"'>" +
		identity_img(i, size) +
		"<img class='onhover' src='" + identity_av_base(i) +
		"'/>";

	if (parts & 1)
		s += san(decodeURIComponent(i.name));
	
	s += "</span></a>";
	
	if (parts & 2)
		s += " <span>&lt;" + san(i.email) + "&gt;</span>";
	
	if (parts & 4)
		s += " <span>" + tz_date(i.git_time.time, i.git_time.offset) +
		     "</span>";
	
	if (!!parts)
		s += "</span>";
	
	return s;
}

function new_ws(urlpath, protocol)
{
	if (typeof MozWebSocket != "undefined")
		return new MozWebSocket(urlpath, protocol);

	return new WebSocket(urlpath, protocol);
}


function aliases(oid)
{
	var irefs = "";

	for (m = 0; m < oid.alias.length; m++) {
		r = oid.alias[m];

		if (r.substr(0, 11) === "refs/heads/")
			irefs += " <span class='inline-branch'>" +
					"<table><tr><td class='tight'>" +
					"<img class='branch' tgt='" +
				san(reponame) + "-" + san(r.substr(11)) +
				"' mtitle='"+
				"<div class=\"putt1\"><img class=\"branch\"></div>" +
				"<div class=\"putt2\">" + i18n("Branch Snapshot") + "</div>" +
				"<div class=\"putt3\"><img class=\"archive\"></div>'>" +
				"</td><td class='tight'><span>" +
				r.substr(11) + "</span></td></tr></table></span>";
		else
			if (r.substr(0, 10) === "refs/tags/")
				irefs += " <span class='inline_tag'>" +
						 "<div class='alias'>" +
						 "<img class='tag oneem' tgt='" + san(reponame) + "-" +
						 san(r.substr(10)) +
							"' mtitle='"+
							"<div class=\"putt1\"><img class=\"tag\"></div>" +
							"<div class=\"putt2\">" + i18n("Tag Snapshot") + "</div>" +
							"<div class=\"putt3\"><img class=\"archive\"></div>'>" +
							"&nbsp;" +
						 r.substr(10) + "</div></span>";
	}
	
	return irefs;
}

function arch_select_handler(e)
{
	var suff, b, newloc = vpath;

	e.stopPropagation();
	e.preventDefault();
	
    if (!vpath || vpath[vpath.length - 1] !== '/')
        newloc += '/';

	newloc += reponame;
	if (reponame[reponame.length - 1] !== '/')
	        newloc += '/';
	newloc += "snapshot/" + e.target.textContent;
	// console.log(newloc);
	window.location = newloc;


	if (!menu_popup_archive)
		return;

	menu_popup_archive.remove();
	menu_popup_archive = null;
}

function create_popup(t, h_px, v_px, clas, title, wid, an, ar, clicker)
{
	var pop, s = "", n;
	
	pop = document.createElement("DIV")

	pop.className = clas;
	pop.style.top = v_px + "px";
	if (h_px)
		pop.style.left = h_px + "px";
	
	if (wid)
		pop.style.width = wid + "px";

	
	if (title)
		s += "<div class='popalias'>" +
			title + "</div>";
	
	if (an.length) {

		s += "<div><ul id='menu-ul'>";
	
		for (n = 0; n < an.length; n++)
				s += "<li id='" + ar[n] + "' tabindex='" + n + "'>" +
					an[n] + "</li>";
		
		s += "</ul></div>";
		
	}
		    
	pop.innerHTML = s;
	t.parentNode.appendChild(pop, null);

	if (an.length) {
		document.getElementById(ar[0]).focus();
		for (n = 0; n < an.length; n++)
			document.getElementById(ar[n]).addEventListener(
											"click", clicker, false);
	}
	setTimeout(function() {
		pop.style.opacity = "1";
	}, 1);

	/* detect loss of focus for popup menu */
	pop.addEventListener("focusout", function(e) {
		/* if focus went to a child (menu item), ignore */
		if (e.relatedTarget &&
		    e.relatedTarget.parentNode.id === "menu-ul")
			return;

		pop.remove();
		pop = null;
	});

	return pop;
}

function archive_click(t)
{
	var v = collect_offsetTop(t.target), tgt, title, h;
	
	if (menu_popup_archive) {
		menu_popup_archive.remove();
		menu_popup_archive = null;
		
		return;
	}
	
	if (t.target.offsetHeight)
		v += t.target.offsetHeight;
	
	h = collect_offsetLeft(t.target) + t.target.offsetWidth;
	
	tgt = t.target.getAttribute("tgt");
	title = t.target.getAttribute("mtitle");
	
	menu_popup_archive =
		create_popup(t.target, h, v, "popup-menu", title,  null,
				[ tgt + ".tar.gz", tgt + ".tar.bz2",
				  tgt + ".tar.xz", tgt + ".zip" ],
				[    "gz",      "bz2",     "xz", "zip" ],
					 arch_select_handler);
}

function clonecopy_click(t)
{
	copy_clipboard("git clone " + san(j.url), t.target);
}

function patch_archive(oid, n)
{
	return "<span><img id='idpa" + n + "' class='patch' tgt='" +
		san(reponame) + "-" + oid +
		"' mtitle='"+
		"<div class=\"putt1\"><img class=\"patch\"></div>" +
		"<div class=\"putt2\">" + i18n("Commit Snapshot") + "</div>" +
		"<div class=\"putt3\"><img class=\"archive\"></div>'>" +
		"</span>";
}

function html_branches(now, count)
{
	var s = "", n;

	s += "<tr><td><table><tr>" +
		"<td class='heading' colspan='2'>" + i18n("Branch") +
		"</td><td class='heading'>" + i18n("Message") +
		"</td><td class='heading'>" + i18n("Author") +
		"</td><td class='heading'>" + i18n("Age") +
		"</td><td></td></tr>";

	for (n = 0; n < branches.length && n < count; n++)
		s += "<tr><td>"+
			 "<div><img id='idsb" + n +
			 	"' class='branch' tgt='" +
				san(reponame) + "-" + san(branches[n].name.substr(11)) +
				"' mtitle='"+
				"<div class=\"putt1\"><img class=\"branch\"></div>" +
				"<div class=\"putt2\">" + i18n("Branch Snapshot") + "</div>" +
				"<div class=\"putt3\"><img class=\"archive\"></div>'>" +
			 "</td><td><a href=\"" +
			makeurl(reponame, "tree", rpath, branches[n].name.substr(11),
					qid, qofs) + "\">" + branches[n].name.substr(11) + "</a>" +
		    "</td><td>" + san(branches[n].summary.msg) +
		    "</td><td>" + identity(branches[n].summary.sig_author, 16, 1) +
		    "</td><td>" + agify(now, branches[n].summary.time) +
		    "</td></tr>";
	
	if (n === count)
		s += "<tr><td colspan=5><a href=\"" +
			makeurl(reponame, "branches", rpath, qbranch, null, null) +
			"\">[...]</a></td></tr>";
	
	s += "</table></td></tr>";

	return s;
}

function html_tags(now, count)
{
	var s = "", n;
	
	if (!tags.length)
		return "";
	
	s += "<tr><td><table><tr><td class='heading' colspan='2'>" + i18n("Tag") +
    "</td><td class='heading'>" + i18n("Message") +
    "</td><td class='heading'>" + i18n("Author") +
    "</td><td class='heading'>" + i18n("Age") +
    "</td><td>" +
    "</td></tr>";

	for (n = 0; n < tags.length && n < count; n++) {
		
		var mm = tags[n].summary.msg_tag;
		//if (!mm)
			//mm = tags[n].summary.msg;
		
		if (tags[n].summary && tags[n].summary.oid && tags[n].summary.oid.oid) {
		
		s += "<tr><td>" +
			"<span><img id='idst" + n + "' class='tag' tgt='" +
				san(reponame) + "-" + san(tags[n].name.substr(10)) +
				"' mtitle='"+
				"<div class=\"putt1\"><img class=\"tag\"></div>" +
				"<div class=\"putt2\">" + i18n("Tag Snapshot") + "</div>" +
				"<div class=\"putt3\"><img class=\"archive\"></div>'>" +
				"</span></td><td><a href=\"" +
			makeurl(reponame, "tree", rpath, qbranch,
				tags[n].summary.oid.oid, qofs) + "\">" +
				tags[n].name.substr(10) + "</a>" +
		"</td><td>" + san(mm) +
		"</td><td>" + identity(tags[n].summary.sig_tagger, 16, 1) +
	    "</td><td>" + agify(now, tags[n].summary.time) +
	    "</td></tr>";
	}
	}
	
	if (n == count)
		s += "<tr><td colspan=5><a href=\"" +
		makeurl(reponame, "tags", rpath, qbranch, null, null) +
		"\">[...]</a></td></tr>";
	
	s += "</table></td></tr>";

	return s;
}

function html_log(l, now, count, next)
{
	var s = "", n;

	s += "<table><tr><td class='heading'>" +
    "</td><td class='heading'>" + i18n("Message") +
    "</td><td class='heading'>" + i18n("Author") +
    "</td><td class='heading'>" + i18n("Age") +
    "</td><td class='heading'>" +
    "</td></tr>";

	if (!l) {
		console.log("html_log: null l\n");
		return "";
	}	
	
	for (n = 0; n < l.length && n < count; n++) {
		var irefs = "", m, r, c;
		
		irefs = aliases(l[n].name);
		
		s += "<tr><td>" + 
		"</td><td class='logmsg'>" + patch_archive(l[n].name.oid, n) + " <a href=\"" +
		    makeurl(reponame, "commit", rpath, null, l[n].name.oid, qofs) +
		    "\">" + san(l[n].summary.msg) + "</a>" + irefs +
		"</td><td>" + identity(l[n].summary.sig_author, 16, 1) +
	     "</td><td>" + agify(now, l[n].summary.time) +
	     "</td><td>" + //j.items[1].log[n].name.substr(10) +
	     "</td></tr>";
	}
	
	if (next && n === count) {
		s += "<tr><td colspan=5><a href='"+
			makeurl(reponame, "log", rpath, null, next.oid, qofs) +
			"'>next</a></td></tr>";
	}
	
	s += "</table>";
	
	return s;
}

function html_commit(j) {
	var s = "";
	
	if (!j.items[0].commit)
		return;
	
	s += "<div><table>";
	s += "<tr><td>" + i18n("Author") + "</td><td>" +
			identity(j.items[0].commit.sig_author, 16, 7) + "</td></tr>";
	s += "<tr><td>" + i18n("Committer") + "</td><td>" +
			identity(j.items[0].commit.sig_commit, 16, 7) + "</td></tr>";
	s += "<tr><td>" + i18n("Tree") + "</td><td><a href=\"" +
		makeurl(reponame, "tree", rpath,
				qbranch, san(j.items[0].commit.oid.oid), qofs) + "\">" +
				san(j.items[0].commit.oid.oid) + "</a>" + "&nbsp;&nbsp;" +
				"<a href=\"" + makeurl(reponame, "patch", rpath,
						qbranch, san(j.items[0].commit.oid.oid), qofs) + "\">" +
						"<img class='rawpatch'> " +
				i18n("Raw Patch") + "</a>"
				"</td></tr>";
	
	s += "<tr><td colspan=2>&nbsp;</td></tr>";
	
	s += "<tr><td colspan=2 class='logtitle'>" +
			patch_archive(j.items[0].commit.oid.oid, 0) +
		    san(j.items[0].commit.msg) + " " +
			aliases(j.items[0].commit.oid) + "</td></tr>";
	if (j.items[0].body)
		s += "<tr><td colspan=2 >" + "<pre class='logbody'>" +
			 san(j.items[0].body) + "</pre></td></tr>";
	s += "</table></div>";
	
	return s + "<div><pre><main role='main'><code id='do-hljs' class=\"diff\">" +
				san(j.items[0].diff) + "</code></main></pre></div>";
}

function filemode3(n)
{
	var s = "";
	
	if (n & 4)
		s += 'r';
	else
		s += '-';
	if (n & 2)
		s += 'w';
	else
		s += '-';
	if (n & 1)
		s += 'x';
	else
		s += '-';
	
	return s;
}

function filemode(n)
{
	var s = "";

	if (n & 16384)
		return "";
	else
		s += '-';

	return s + filemode3(n >> 6) + filemode3(n >> 3) + filemode3(n);
}

function html_tree(j, now)
{
	var n, bi = 0, s = "", mo = rmode;
	
	if (mo !== "tree" && mo !== "blame")
		mo = "tree";

	if (!blog_mode && j.items[0] && j.items[0].tree) {
	
		var s = "<div class='jg2-tree'><pre><table><tr>" +
					"<td class='heading'>" + i18n("Mode") + "</td>" +
					"<td class='headingr'>" + i18n("Size") + "</td>" +
					"<td class='heading'>" + i18n("Name") + "</td>" +
					"</tr>";
		var t = j.items[0].tree;
	
		for (n = 0; n < t.length; n++) {
			if ((parseInt(t[n].mode, 10) & 0xe000) === 0xe000)
				s += "<tr><td>&nbsp</td><td>&nbsp</td>" +
				     "<td class='dl-dir'><img class='submodule'>&nbsp;" +
			         san(t[n].name) + "</td></tr>";
			else
			
			if (parseInt(t[n].mode, 10) & 16384)
				s += "<tr><td>" + filemode(parseInt(j.items[0].tree[n].mode, 10)) + "</td>" +
			     "<td>&nbsp</td><td class='dl-dir'><img class='folder'>&nbsp;" +
			     "<a class='noline' href='" +
				 makeurl(reponame, mo, (rpath != null ? rpath + '/' : "") +
						 san(t[n].name), qbranch, qid, qofs) +
				 "'>" + san(t[n].name) + "</a></td></tr>";
			else
				s += "<tr><td>" +
					 filemode(parseInt(j.items[0].tree[n].mode, 10)) +
					 "</td>" + "<td class='r'>" +
					 parseInt(j.items[0].tree[n].size, 10) +
					 "</td><td class='dl-file'><a class='noline' href='" +
					 makeurl(reponame, mo, (rpath != null ? rpath + '/' : "") +
							 san(j.items[0].tree[n].name), qbranch, qid, qofs) +
					 "'>" + san(j.items[0].tree[n].name) + "</a></td></tr>";
		}
	
		s += "</table></pre></div>";
		bi = 1;
	}
	
	if (blog_mode)
		bi = 1;
	
	if (j.items[bi] && j.items[bi].blob) {
		
		if ((rpath && rpath.substr(rpath.length - 3) === '.md') ||
			(rpath && rpath.substr(rpath.length - 4) === '.mkd') ||
			bi) {
			
			if (!blog_mode)
			s += "<div class='inline'><div class='inline-title'>" +
			san(j.items[bi].blobname) +
			     "</div>";
			s += "<table><tr>"+
			"<td class='doc' id='do-showdown'>" + san_nq(j.items[bi].blob) +
			"</td></tr></table></div>";
		} else {
			var optclass = "";
			
			if (rpath && rpath.substr(rpath.length - 4) === ".txt")
				optclass = "class=\"plaintext\"";
	
			s += "<table>";
			
			if (j.items[bi + 1] && j.items[bi + 1].contrib) {
				var ct = j.items[bi + 1].contrib, c = ct.length;
				if (c > 20)
					c = 20;
				s += "<tr><td colspan='2'><table><tr>";
				for (n = 0; n < c; n++) {
					s += "<td>" +
					identity(j.items[bi + 1].blame[ct[n].o].sig_final, 24, 0) +
							"</td>";
				}
				s += "</tr></table></td></tr>";
			}
			
			s += "<tr><td><pre><code>"+
		    "<div id='jglinenumbers' class='jglinenumbers'>"+
		    "</div></code></pre></td>" +
			"<td class='doc'><pre><code id=\"do-hljs\" " + optclass +
				">" + san(j.items[bi].blob) +
			"</code></pre></td></tr></table>";
		}
	}
	
	if (j.items[bi] && j.items[bi].bloblink) {
		var l = j.items[bi].bloblink;
		
		s += "<div class='inline'><div class='inline-title'>" +
		san(j.items[bi].blobname) +
		     "</div>" +
			"<table><tr>"+
		"<td class='doc'>";
		
		if (l.substr(l.length - 4).toLowerCase() === ".png" ||
			l.substr(l.length - 4).toLowerCase() === ".jpg" ||
			l.substr(l.length - 4).toLowerCase() === ".ico" ||
			l.substr(l.length - 5).toLowerCase() === ".jpeg") {

			s += "<img src=\"" + san(l) + "\">";
		} else
			s += "<a href=\"" + san(l) + "\" download>" + i18n("Download") + "</a>";
			
		s += "</td></tr></table></div>";
	}

	return s;
}

function html_repolist(j, now)
{
	var n, s = "";

	if (j.items[0] && j.items[0].repolist) {
	
		var s = "<div class='jg2-repolist'><table><tr>"+
					"<td class='heading'>" + i18n("Name") + "</td>" +
					"<td class='heading'>" + i18n("Description") + "</td>" +
					"<td class='heading'>" + i18n("Owner") + "</td>" +
					"<td class='heading'>" + i18n("URL") + "</td></tr>";
		var t = j.items[0].repolist;
	
		for (n = 0; n < t.length; n++)
				s += "<tr><td class='dl-dir'><img class='repo oneem'>" +
					"&nbsp;<a class='noline'href='" +
				 makeurl(t[n].reponame, null, null, null, null, null) +
				 "'>" + san(t[n].reponame) + "</a></td>" +
			     "<td>" + san(t[n].desc) + "</td>" +
			     "<td>" + identity(t[n], 16, 1) + "</td>" +
			     "<td>" + san(t[n].url) + "</td>" +
			     "<td>&nbsp</td></tr>";
	
		s += "</table></div>";
	}

	return s;
}

function display_summary(j, now)
{
	var n, s = "<table>";

    s += html_branches(now, 10)
	     
	s += "<tr><td colspan=5>&nbsp;</td></tr>";

    s += html_tags(now, 10);
	
	s += "<tr><td colspan=5>&nbsp;</td></tr>";
	
	s += html_log(j.items[1].log, now, 10);
	
	s += "<tr><td colspan=5><a href=\"" +
		makeurl(reponame, "log", rpath, qbranch, null, null) +
		"\">[...]</a></td></tr>";

	s += "</table>";

	return s;
}

var doc_dir = "";

var sd_ext_plain = function () {
  var ext1 = {
    type: 'output',
    regex: '<img\ src=\"\./([^\"]*)',
    replace: '<img\ \ src=\"' + makeurl(reponame, "plain", doc_dir + '$1', qbranch, qid, qofs)
  };
  var ext2 = {
    type: 'output',
    regex: '<img\ src=\"(?!http:\/\/|https:\/\/|\/)([^\"]*)',
    replace: '<img\ \ src=\"' + makeurl(reponame, "plain", doc_dir + '$1', qbranch, qid, qofs)
  };
  var ext3 = {
    type: 'output',
    regex: '<img\ src=\"\/([^\"]*)',
    replace: '<img\ src=\"' + makeurl(reponame, "plain", '$1', qbranch, qid, qofs)
  };
  var ext4 = {
    type: 'output',
    regex: '<a\ href=\"\./([^\"]*)',
    replace: '<a\ class=\"blogintlink\"\ href=\"' + makeurl(reponame, "tree", doc_dir + '$1', qbranch, qid, qofs)
  };
  var ext5 = {
    type: 'output',
    regex: '<a\ href=\"http[s]*://([^\"]*)',
    replace: '<a\ class=\"blogextlink\"\ href=\"https://' + '$1'
  };
  
  return [ext1, ext2, ext3, ext4, ext5];
}

var ws, j;
var last_mm, blametable, blamesel, blameotron;

function blameotron_handler(e)
{
	}

function blame_normal(d)
{
	if (!d)
		return;
	
	var a = document.getElementsByClassName(d);
	var hunks = j.items[1].blame.length, hunk = parseInt(d.substr(6), 10);

	for (m = 0; m < a.length; m++)
		a[m].style.backgroundColor =
			"rgba(" + (128 + (((hunk) * 128) / hunks)) +
			   ", " + (128 + (((hunk) * 128) / hunks)) +
			   ", " + (128 + (((hunk) * 128) / hunks)) +
			   ", 0.3)";
}

function blameotron_revert(e)
{
	var dest = e.target.parentNode.getAttribute("dest");
	window.location = dest;
}

function blame_mousemove(e)
{
	var d = new Date(), t = d.getTime(), y = 0, n, m;

	/* check the mouse only at 20Hz */
	
	if (blamesel && last_mm && t - last_mm < 50)
		return;

	last_mm = t;
	
	/*
	 * This slightly avant-garde api is used because we face a problem...
	 * the blame divs need to be behind the text (the highlight is already
	 * in front of it) but need to convert mousemoves to blame divs.
	 *
	 * We can't get the mousemoves by removing pointer-events from things in
	 * front, because the user may want to highlight text for cut-and-paste
	 * or click the line number links.
	 */
	
	var x;
	
	if (x < 80)
		x = 80;
	else
		x = e.clientX;
	
	var elements = document.elementsFromPoint(x, e.clientY), i;
	var hunks = j.items[1].blame.length;
	
	for (m = 0; m < elements.length; m++)
		for (n = 0; n < elements[m].classList.length; n++) {
			if (elements[m].classList[n] === "popup-blameotron")
				return;
			if (elements[m].classList[n] === "putt")
				return;
		}
	
	
	for (m = 0; m < elements.length; m++) 
		for (n = 0; n < elements[m].classList.length; n++) 
			if (elements[m].classList[n].substr(0, 6) === "bhunk-") {
				
				if (blamesel == elements[m].classList[n])
					return;
				
				blame_normal(blamesel);
				if (blameotron) {
					blameotron.remove();
					blameotron = null;
				}
				
				blamesel = elements[m].classList[n];
				var a = document.getElementsByClassName(blamesel);
				hunk = parseInt(blamesel.substr(6), 10);
				for (i = 0; i < a.length; i++)
					a[i].style.backgroundColor =
						"rgba(" + (128 + (((hunk) * 64) / hunks)) +
						   ", " + (128 + (((hunk) * 64) / hunks)) +
						   ", " + (192 + (((hunk) * 128) / hunks)) +
						   ", 0.5)";
				
				if (!blameotron) {
					var thehunk = j.items[1].blame[hunk], bow,
						botr_width = 250, bp = rpath, devolve, dl = "";
					
					bow = blametable.offsetWidth;
					if (bow > window.pageXOffset + window.innerWidth)
						bow = window.pageXOffset + window.innerWidth;
					
					bow -= botr_width + 32;
					
					/* original filepath may have been different */
					if (thehunk.op)
						bp = thehunk.op;
					
					devolve = makeurl(reponame, "blame", bp,
							null, thehunk.orig_oid.oid, null) +
							"#n" + thehunk.ranges[parseInt(
							 elements[m].getAttribute("r"), 10)].o;
					
					if (thehunk.orig_oid.oid != qid)
						dl = "<a class='blameotron-revert' dest=\"" + devolve +
						"\"><img class=\"devolve\"></a>&nbsp;";

					blameotron = create_popup(elements[m],
								bow,
								collect_offsetTop(elements[m]),
								"popup-blameotron",
								"<div class=\"putt\"><div class=\"putt1_blame\">"+
									"<img class=\"blame hflip\"></div>" +

								"<div class=\"putt2\">" + dl +
								
									thehunk.sig_final.name + "<br>" +
									agify(new Date().getTime() / 1000,
										thehunk.sig_final.git_time.time) +
										"</div>" +

								"<div class=\"putt4\">" +
								"<a class='blameotron-revert' dest=\"" +
									makeurl(reponame, "commit", null,
											null, thehunk.final_oid.oid, null) +
										"\">" +
										thehunk.log_final + "</a>" +
								"</div></div>" +
										
								"<div class=\"putt3_blame\"><span class='gravatar64'>" +
									identity_img(thehunk.sig_final, 64) +
									"</span></div>",
									botr_width, [ ], [ ], blameotron_handler);
				}
				
				var elems = document.getElementsByClassName("blameotron-revert"), n;
				for (n = 0; n < elems.length; n++)
					elems[n].addEventListener("click", blameotron_revert.bind(elems[n]));
				
				return;
			}
}

function blame_mouseout(e)
{	
	if (!blamesel)
		return;
	
	var x;
	
	if (x < 80)
		x = 80;
	else
		x = e.clientX;
	
	var elements = document.elementsFromPoint(x, e.clientY), m, n;
	var hunks = j.items[1].blame.length;
	
	for (m = 0; m < elements.length; m++)
		for (n = 0; n < elements[m].classList.length; n++) {
			if (elements[m].classList[n].substr(0, 6) === "bhunk-")
				return;
			
			if (elements[m].classList[n] === "popup-blameotron")
				return;
			if (elements[m].classList[n] === "putt")
				return;
		}

	blame_normal(blamesel);
	blamesel = null;
	
	if (blameotron) {
		blameotron.remove();
		blameotron = null;
	}
}


function goh_fts_choose()
{
	var ac = document.getElementById("searchresults");
	var inp = document.getElementById("gohsearch");
	var jj, n, m, s = "", x, lic = 0, hl, re;

//	sr.style.width = (parseInt(sr.parentNode.offsetWidth, 10) - 88) + "px";
//	sr.style.opacity = "1";
	
	inp.blur();
	ac.style.opacity = "0";
	window.location = makeurl(reponame, "search", rpath, qbranch, null, null,
						encodeURIComponent(inp.value));
}

function goh_ac_select(e)
{
	var t;
	
	if (e) {
		
		
		 t = e.target;
		
		// console.log(t);
	
		while (t) {
			if (t.getAttribute && t.getAttribute("string")) {
				document.getElementById("gohsearch").value =
						t.getAttribute("string");
	
				goh_fts_choose();
				return;
			}
	
			t = t.parentNode;
		}
	} else
		goh_fts_choose();
}

function goh_search_input()
{
	//document.getElementById("searchresults").style.display = "block";
	document.getElementById("searchresults").style.opacity = "1";

	/* detect loss of focus for popup menu */
	document.getElementById("gohsearch").
			addEventListener("focusout", function(e) {
			/* if focus went to a child (menu item), ignore */
			if (e.relatedTarget &&
			    e.relatedTarget.parentNode.id === "searchresults")
			return;
			
			console.log("focusout");

			document.getElementById("searchresults").style.opacity = "0";
		//	document.getElementById("searchresults").style.display = "none";
	});
	
	
	var xhr = new XMLHttpRequest();

	xhr.onopen = function(e) {
		xhr.setRequestHeader('cache-control', 'max-age=0');
	}
	xhr.onload = function(e) {
		var jj, n, s = "", x, mi = 0, lic = 0;
		var inp = document.getElementById("gohsearch");
		var ac = document.getElementById("searchresults");
		
		//console.log(xhr.responseText);
		jj = JSON.parse(xhr.responseText);
		
		//console.log("jj.indexed: " + jj.indexed);
		
		switch(parseInt(jj.indexed, 10)) {
		case 0: /* there is no index */
			break;

		case 1: /* yay there is an index */
		
			s += "<ul id='menu-ul'>";
			for (q = 0; q < jj.items.length; q++) {
				if (jj.items[q].ac) {
					lic = jj.items[q].ac.length;
					for (n = 0; n < lic; n++) {
						var cla = "acitem", m = 0;
						
						if (jj.items[q].ac[n] && jj.items[q].ac[n].elided)
							cla += " eli";
						
						if (jj.items[q].ac[n]) {
							m = parseInt(jj.items[q].ac[n].matches, 10);
							if (!m) {
								cla += " virt";
								m = parseInt(jj.items[q].ac[n].agg, 10);
							}
						}
						
						s += "<li id='mi_ac" + mi + "' string='" +
									san(jj.items[q].ac[n].ac) + 
									"'><table class='" + cla +
									"'><tr><td>" +
									san(jj.items[q].ac[n].ac) +
									"</td><td class='rac'>" + m +
									"</td></tr></table>" + "</li>";
						
						mi++;
					}
				}
				if (jj.items[q].search) {			
					for (n = 0; n < jj.items[q].search.length; n++)
						s += "<li>" +
						  parseInt(jj.items[q].search[n].matches, 10) +
						  "</td><td>" +  san(jj.items[q].search[n].fp) +
						  "</li>";
				}
			}
			s += "</ul>";
			
			 if (!lic) {
					//s = "<img class='noentry'>";
					inp.className = "nonviable";
					ac.style.opacity = "0";
				 } else {
					 inp.className = "viable";
					 ac.style.opacity = "1";
				 }
			
			break;
			
		default:
			
			/* an index is being built... */
			
			s = "<table><tr><td><img class='spinner'></td><td>" +
				"<table><tr><td>Indexing</td></tr><tr><td>" +
				"<div id='bar1' class='bar1'>" +
				"<div id='bar2' class='bar2'>" +
				jj.index_done + "&nbsp;/&nbsp;" + jj.index_files +
				"</div></div></td></tr></table>" +
				"</td></tr></table>";
		
			setTimeout(goh_search_input, 300);
		
			break;
		}
		
		document.getElementById("searchresults").innerHTML = s;
	
		for (n = 0; n < mi; n++)
			if (document.getElementById("mi_ac" + n))
				document.getElementById("mi_ac" + n).
					addEventListener("click", goh_ac_select);

		inp.addEventListener("keydown",
				function(e) {
			var inp = document.getElementById("gohsearch");
			var ac = document.getElementById("searchresults");
			if (e.key === "Enter" && inp.className === "viable") {
				goh_ac_select();
				ac.style.opacity = "0";
			}
		}, false);
		
		if (jj.index_files) {
			document.getElementById("bar2").style.width =
				((150 * jj.index_done) / (jj.index_files + 1)) + "px";
		}
	}
	
	xhr.open("GET", makeurl(reponame, "ac", null, qbranch, null, null,
			 document.getElementById("gohsearch").value));
	xhr.send();
}

function qindexing_update()
{
	var xhr = new XMLHttpRequest();

	xhr.onopen = function(e) {
		xhr.setRequestHeader('cache-control', 'max-age=0');
	}
	xhr.onload = function(e) {
		var sp1 = xhr.responseText.split("class=\"hidden-tiger\">"),
		    sp2 = sp1[1].split("</div>");
		
		console.log(sp2[0]);
		jj = JSON.parse(sp2[0]);
		
		search_results(jj);
	}
	
	xhr.open("GET", makeurl(reponame, "search", null, qbranch, null, null,
			 qsearch));
	xhr.send();
}

function search_results(j)
{
	var s = "", qi = document.getElementById("qindexing");
	var now = new Date().getTime() / 1000;

	switch(parseInt(j.indexed, 10)) {
	
	case 1:
	
		s += "<tr><td class='searchfiles'><main role=\"main\"><table>";

			if (j.items[0] && j.items[0].search && j.items[0].search[0]) {
				lic = j.items[0].search.length;						
				for (n = 0; n < lic; n++) {
					var link = "";
					
					link += j.items[0].search[n].fp;
					
					s += "<tr>" +
						 "<td class='rpathinfo'>" + j.items[0].search[n].matches +
						 "</td><td class='path'><a href=\"" +
						 makeurl(reponame, "tree", san(link),
								 qbranch, null, null, qsearch) + "\">" +
						 san(link) +
					"</a></td></tr>";
				}
			}
		
		s += "</main></td><td>" + html_tree(j, now) + "</td></tr>";
		break;
		
	default:
				/* an index is being built... */
				
				s = "<table><tr><td><img class='spinner'></td><td>" +
					"<table><tr><td>Indexing</td></tr><tr><td>" +
					"<div id='bar1' class='bar1'>" +
					"<div id='bar2' class='bar2'>" +
					j.index_done + "&nbsp;/&nbsp;" + j.index_files +
					"</div></div></td></tr></table>" +
					"</td></tr></table>" +
					"</div></td></tr>";
			
				setTimeout(qindexing_update, 300);
			
				break;
	}
	
	if (qi) {
		qi.innerHTML = s;
		return "";
	}

	return s;
}

function display(j)
{
	var url = window.location.pathname, q, n,
		s = "<table class='repobar'><tbody class='repobar'>",
		now = new Date().getTime() / 1000;

	/*
	 * /vpath/reponame/mode/repopath[?h=branch][id=xxx]
	 */
	
	if (url.substr(0, j.vpath.length) === j.vpath)
		url = url.substr(j.vpath.length + 1);
	
	q = url.split('/');
	
	relpre_no_mode = j.vpath + '/';
	
	if (!blog_mode && reponame) {
		var do_aliases = null, s1 = "", s2 = "", s3 = "", s4 = "";
		
		s += "<tr class='repobar'><td class='repobar'><div class='repobar'>" +
		     "<table><tr><td><span class='reponame'>" +
			 "<a href='" + makeurl() + "'>" +
			 "<img class='repolist'></a>&nbsp;&nbsp;<a href=\"" +
		     makeurl(reponame, "summary", null, null, null, null) + "\">" +
		     reponame + "</a>&nbsp;&nbsp;</span></td><td class='tight'>";

		s += "<table>";
		
		if (j.desc)
			s += "<tr><td class='tight info' colspan='2'>" +
				 "<img class='info'>&nbsp;" + san(j.desc) + "</td></tr>";
	
		s += "<tr>";
		
		if (j.owner)
			s += "<td class='tight'> " + identity(j.owner, 16, 1) + "</td>";
		else
			s += "<td></td>";
			
		if (j.url)
			s += "<td class='tight'> " + 
				 "<img class='copy' id='clonecopy'>&nbsp;<i>git clone " +
				 san(j.url) + "</i></td>";
		else
			s += "<td></td>";
			
		s += "</tr></table>" +
			 "</td></tr></tbody></table>" +
			 "</div></td>" +
			 
			 "<td class='rt'><div class='search'>" +
			 "<table><tr><td rowspan='2'><img class='eyeglass'></td>" +
			 "<td class='searchboxtitle'>Fulltext search<br>" +
			 "<input type='text' id='gohsearch' name='gohsearch' maxlength='80'>" +
			 "<div class='searchresults' id='searchresults'></div>" +
			 "</td></tr></table></div></td>" +
			 
			 "</tr>" +
			 "<tr class='tabbar'><td class='tabbar'>";
			
		if (q[1]) {
			
			switch (q[1]) {
			case "commit":
				if (j.items[0].commit)
				do_aliases = j.items[0].commit.oid;
				break;
			case "log":
				do_aliases = j.items[0].log[0].name;
				s2 = " class=\"selected\" ";
				break;
			case "tags":
			case "branches":
				break;
			case "summary":
				s1 = " class=\"selected\" ";
				break;
			default:
				if (q[1] === "blame")
					s4 = " class=\"selected\" ";
				else
					s3 = " class=\"selected\" ";
				do_aliases = j.items[0].oid;
				break;
			}
		} else {
			do_aliases = j.items[0].oid;
			s3 = " class=\"selected\" ";
		}
		
		
		s += "<div class='tabbar'>";
		
		s += "<div class='tabs'><ul>";
		
		s += "<li " + s1 + "><a href=\"" +
			makeurl(reponame, "summary", null, null, null, null) +
			"\"><img class='summary'>" + i18n("Summary") + "</a></li>";
		
		s += "<li " + s2 + "><a href=\"" +
			makeurl(reponame, "log", rpath, qbranch, qid, qofs) +
			"\"><img class=\"commits\">" + i18n("Log") + "</a></li>";
		
		s += "<li " + s3 + "><a href=\"" +
			makeurl(reponame, "tree", rpath, qbranch, qid, qofs) +
			"\"><img class='tree'>" + i18n("Tree") + "</a></li>";
		
		if (j.f & 1) { /* capable of blame */
			s += "<li " + s4 + "><a href=\"" +
				makeurl(reponame, "blame", rpath, qbranch, qid, qofs) +
				"\"><img class='blame'>" + i18n("Blame") + "</a></li>";
		}
		
		s += "</ul></div>&nbsp;";
		
		s += "<div class='rpathname'>";
		if (do_aliases)
			s += aliases(do_aliases) + "&nbsp;";
		
		if (rpath) {
			var e = rpath.split('/'), agg = "", mo = rmode;
			
			if (mo !== "tree" && mo !== "blame")
				mo = "tree";

			s += "<a href=\"" +
				makeurl(reponame, mo, "", qbranch, qid, qofs) + "\">" +
				i18n("root") + "</a>";
			
			s += "<span class='repopath'>";
			
			for (n = 0; n < e.length; n++) {
				if (e[n]) {
					if (n)
						agg += "/";
					agg += e[n];
					if (n === e.length - 1)
						s += " / " + e[n];
					else
						s += " / <a href=\"" +
							makeurl(reponame, mo, agg, qbranch, qid, qofs) + "\">" +
							e[n] + "</a>";
				}
			}
			s += "</span>";
		}
		s += "</div>";
		
		s += "</div></td></tr>";
	}

	if (q.length === 1 && reponame)
		relpre_no_mode += q[0] + '/';
	
	relpre_no_mode += q[0] + '/';
	
	if (!blog_mode && q.length <= 2 && !reponame) {
			s += "<tr><td class='repolist'>" + html_repolist(j, now) +
			"</td></tr>";
		document.getElementById("result").innerHTML = s + "</table>";
		return;
	}

	if (j.items[0].error) {
		s += "<tr><td><span class='error'><img class='warning'>" + san(j.items[0].error) + "</span></td></tr>";
	} else
	
	if (!blog_mode && q[1])
		switch (q[1]) {
		case "log":
			s += "<tr><td><main role=\"main\">" +
				html_log(j.items[0].log, now, 50, j.items[0].next) + "</main></td></tr>";
			break;
		case "commit":
			s += "<tr><td><main role=\"main\">" + html_commit(j) + "</main></td></tr>";
			break;
		case "tags":
			s += "<tr><td><main role=\"main\">" + html_tags(now, 999) + "</main></td></tr>";
			break;
		case "branches":
			s += "<tr><td><main role=\"main\">" + html_branches(now, 999) + "</main></td></tr>";
			break;
		case "summary":
			s += "<tr><td><main role=\"main\">" + display_summary(j, now) + "</main></td></tr>";
			break;

		case "search":
			s += "<tr><td><div id=qindexing>" +
					search_results(j) +
				 "</div></td></tr>"
					;
			break;
		default:
			s += "<tr><td><main role=\"main\">" + html_tree(j, now) + "</main></td></tr>";
			break;
		}
	else
		s += "<tr><td><main role=\"main\">" + html_tree(j, now) + "</main></td></tr>";

	s += "</table>";
	
	document.getElementById("result").innerHTML = s;



	/* add class-based events now the objects exist */

	document.getElementById("gohsearch").addEventListener("input",
								goh_search_input, false);
	
//	elems = document.getElementsByClassName("inline-identity");
	//for (n = 0; n < elems.length; n++)
		//elems[n].addEventListener("error", img_err_retry);
	
	if (j.f & 2) {
		var elems = document.getElementsByClassName("archive");
		for (n = 0; n < elems.length; n++)
			elems[n].addEventListener("click", archive_click);
		
		elems = document.getElementsByClassName("tag");
		for (n = 0; n < elems.length; n++)
			elems[n].addEventListener("click", archive_click);
		
		elems = document.getElementsByClassName("branch");
		for (n = 0; n < elems.length; n++)
			elems[n].addEventListener("click", archive_click);
		
		elems = document.getElementsByClassName("patch");
		for (n = 0; n < elems.length; n++)
			elems[n].addEventListener("click", archive_click);
	}
	
	{
		var hh = document.getElementById("clonecopy");
		if (hh)
			hh.addEventListener("click", clonecopy_click);
	}
	
	var name = rpath;
	if ((name && name.substring(name.length - 3) === ".md") ||
		(name && name.substring(name.length - 4) === ".mkd") ||
		 ((!rmode || rmode === "tree" || rmode === "blame") &&
		   document.getElementById("do-showdown"))) {
		
		doc_dir = "";
		if (name) {
			n = name.lastIndexOf("/");
			if (n > 0)
				doc_dir = name.substr(0, n + 1);
		}
				
		showdown.extension('sd_ext_plain', sd_ext_plain);
		
		var conv = new showdown.Converter({extensions: ['sd_ext_plain']}), n;
    	var hh = document.getElementById("do-showdown");
		/* blog formatting? */
		var hd = hh.textContent.substring(0, 1024), sp, bf = 0, hdl = 0,
				hdhtml = "", hdhtmle = "";
		
		sp = hd.split('\n');
		if (sp[0].substr(0, 1) == '%' && sp[1].substr(0, 1) == '%' &&
			sp[2].substr(0, 1) == '%') {
			bf = 1;
			hdl = sp[0].length + sp[1].length + sp[2].length + 3;
			hdhtml = "<main role=\"main\"><span class=\"blogtitle\">" + san(sp[0].substring(1)) +
					 "</span><br>" +
					 "<span class=\"blogdate\">" +
						san(sp[2].substring(1)) + "</span><p>";
			hdhtmle = "</main>";
		}

		conv.setOption('tables', '1');
		conv.setFlavor('github');

		if (bf)
			hh.innerHTML = hdhtml + conv.makeHtml(hh.textContent.substr(hdl)) + hdhtmle;
		else
			hh.innerHTML = "<main role=\"complementary\">" + conv.makeHtml(hh.textContent) + "</main>";

	} else
	
		if (typeof hljs !== "undefined") {
			var hh = document.getElementById("do-hljs");
			if (hh) {
				
				/* workaround to stop hljs overriding the code class
				 * restriction */
				
				if (hh.className)
					hljs.configure({
						tabReplace: "        ",
						languages: [ hh.className ]
					})
				else
					hljs.configure({
						tabReplace: "        "
					})
				
				hljs.highlightBlock(hh);
				
			//	if (qsearch)
				//	hh = goh_search_highlight(hh, qsearch);

				var e = document.getElementById("jglinenumbers"),
					count, n = 1, sp;
				if (e) {
					e.onclick = line_range_click;
					
//					e.style.pointerEvents = "none";

					sp = hh.textContent.split('\n');
				count = sp.length;
				if (sp[sp.length - 1].length === 0)
					count--;

				while (n <= count) {
					var lin = document.createElement("a");
					lin.id = "n" + n;
					lin.href = "#n" + n;
					lin.textContent = (n++) + "\n";
						e.appendChild(lin);
					}
				}

				var top = window.getComputedStyle(hh, null).
						getPropertyValue("padding-top");

				if (e)
					e.style.paddingTop = top - 3;
				
				/* blame ... */

				if (j.items[1] && j.items[1].blame) {
					var l1, l2, etr, r, m, hunk, hunks, lofs, etable;

					blametable = find_parent_of_type(e, "table");
					
					blametable.addEventListener("mousemove", blame_mousemove, false);
					blametable.addEventListener("mouseout", blame_mouseout, false);
					
					hunks = j.items[1].blame.length;
					for (hunk = 0; hunk < hunks; hunk++) {
					
						r = j.items[1].blame[hunk].ranges;
						
						if (r) {
							for (n = 0; n < r.length; n++) {
		
								m = document.createElement("DIV");
								
								l1 = r[n].f;
								l2 = l1 + r[n].l - 1;
								
								e = document.getElementById('n' + l1);
								
								m.className = "blamed-lines";
								m.style.bottom = e.style.bottom;
								m.style.top = collect_offsetTop(e) + 'px';
								
								m.classList.add('bhunk-' + hunk);
								m.setAttribute("r", n);
			
								/* we will tack the highlight div at the parent tr */
								etr = e.parentNode;//find_parent_of_type(e, "table");
	
								lofs = (hunk * 72) / hunks;
								m.style.width = (blametable.offsetWidth - lofs) + 'px';
								m.style.left = lofs + 'px';
								m.style.height = ((l2 - l1 + 1) * e.offsetHeight) + 'px';
	
								etr.insertBefore(m, etr.firstChild);
								m.style.backgroundColor =
									"rgba(" + (128 + (((hunk) * 128) / hunks)) +
									   ", " + (128 + (((hunk) * 128) / hunks)) +
									   ", " + (128 + (((hunk) * 128) / hunks)) +
									   ", 0.3)";
		
							}
						}
					}
				}	
	    	}
	    }

}

function parse_json_reflist(j)
{
	var n;
	
	branches.length = 0;
	tags.length = 0;
	
	for (n = 0; n < j.items[0].reflist.length; n++) {
		var l = j.items[0].reflist[n];
		if (l.name.substr(0, 11) === "refs/heads/")
			branches.push(l);
		else
			if (l.name.substr(0, 10) === "refs/tags/") {
				if (l.summary.sig_tagger &&
				    l.summary.sig_tagger.git_time)
					l.summary.time = l.summary.sig_tagger.git_time.time;
				if (!l.summary.sig_tagger && l.summary.sig_author)
					l.summary.sig_tagger = l.summary.sig_author;
				if (!l.summary.msg_tag && l.summary.msg)
					l.summary.msg_tag = l.summary.msg;
				tags.push(l);
			}
	}
	
	branches.sort(comp_reftime);
	tags.sort(comp_reftime_tag);
}

function parse_json(j)
{
	var n, m, u = window.location.pathname, vm, rnl, rml;
	
	j_avatar = j.avatar;
	vpath = j.vpath;
	reponame = j.reponame;
	rmode = null;
	qid = null;
	qbranch = null;
	qofs = null;
	qsearch = null;
	jf = j.f;
	
	m = vpath.length;
	if (!m)
	if (m > 1) {
		m += 1;
		if (vpath[vpath.length - 1] === '/')
			m--;
	}
	if (!reponame.length) {
		reponame = null;
		rnl = 0;
	} else
		rnl = reponame.length + 1;
	
	vm = m;
	if (reponame)
		m += reponame.length;
	
	rmode = u.substr(m + 1);
	if (rmode.length === 0) {
		rmode = null;
		rml = 0;
	} else {
		n = rmode.indexOf('/');
		if (n >= 0) {
			rmode = rmode.substr(0, n);
		} else {
			n = rmode.indexOf('?');
			if (n >= 0) {
				rmode = rmode.substr(0, n);
			}
		}
		rml = rmode.length + 1;
	}

	rpath = u.substr(vm + rnl + rml);
	
	m = rpath.indexOf('?');
	if (m >= 0)
		rpath = rpath.substr(0, m - 1);
	
	if (rpath.substr(rpath.length - 1) === "/")
		rpath = rpath.substr(0, rpath.length - 1);
	
	if (rpath === "")
		rpath = null;
	
	u = window.location.href;
	n = u.indexOf('?');

	if (n >= 0) {
		
		u = u.substr(n + 1);
	
		n = u.indexOf("id=");
		if (n >= 0)
			qid = u.substr(n + 3, 40);
		
		n = u.indexOf("h=");
		if (n >= 0) {
			m = u.substr(n + 2).indexOf('&');
			if (m > 0)
				qbranch = u.substr(n + 2, m);
			else
				qbranch = u.substr(n + 2);
		}
		
		n = u.indexOf("ofs=");
		if (n >= 0) {
			m = u.substr(n + 4).indexOf('&');
			if (m > 0)
				qofs = u.substr(n + 4, m);
			else
				qofs = u.substr(n + 4);
		}
		
		n = u.indexOf("q=");
		if (n >= 0) {
			m = u.substr(n + 2).indexOf('&');
			if (m > 0)
				qsearch = decodeURIComponent(u.substr(n + 2, m));
			else
				qsearch = decodeURIComponent(u.substr(n + 2));
		}
	}

	alang = j.alang;
	
	if (alang) {
		var a = alang.split(","), n;
		
		for (n = 0; n < a.length; n++) {
			var b = a[n].split(";");
			switch (b[0]) {
			case "ja":
				i18n.translator.add(JSON.parse(lang_ja));
				n = a.length;
				loca = b[0];
				break;
			case "zh_TW":
			case "zh_HK":
			case "zh_SG":
			case "zh_HANT":
			case "zh-TW":
			case "zh-HK":
			case "zh-SG":
			case "zh-HANT":
				i18n.translator.add(JSON.parse(lang_zht));
				n = a.length;
				loca = b[0];
				break;
			case "zh":
			case "zh_CN":
			case "zh_HANS":
			case "zh-CN":
			case "zh-HANS":
				i18n.translator.add(JSON.parse(lang_zhs));
				n = a.length;
				loca = b[0];
				break;
			case "en":
			case "en_US":
			case "en-US":
				n = a.length;
				loca = b[0];
				break;
			}
		}
	}
	
	blog_mode = !!(j.f & 4);
	
	if (j.items[0].reflist)
		parse_json_reflist(j);
}

window.addEventListener("load", function() {
	//console.log("load");

	line_range_highlight(1);
}, false);

document.addEventListener("DOMContentLoaded", function() {
	var init = document.getElementById("initial-json");

//	ws = new_ws(get_appropriate_ws_url(window.location.pathname), "lws-gitws");
	 
		if (init) {
			console.log("parsed initial json" + init.textContent);
			j = JSON.parse(init.textContent);
			parse_json(j);
			display(j);
			//init.remove();
		}
		
		var st = document.getElementById("gitohashi-stats");
		
		if (st) {
			var now = new Date().getTime() / 1000;
			s = "<table><tr><td rowspan='2'><img class='stats'></td>" +
				"<td colspan='" + j.items.length + "'>";

			s += i18n("Page fetched %{pf} ago, creation time: %{ct}ms " +
				 "(vhost etag hits: %{ve}%, cache hits: %{ch}%)",
					  {
					    pf:agify(j.gen_ut, now),
					    ct:Math.round(j.g / 1000),
					    ve:parseInt(j.ehitpc, 10),
					    ch:parseInt(j.chitpc, 10)
					  }
					);			
			
			s += "</td></tr><tr>";
			
			for (n = 0; n < j.items.length; n++) {
				
				if (j.items[n].s) {
				s += "<td><img class=\"cache\">JSON " + (n + 1) +
					": ";
					s += i18n("Created %{pf} ago, creation time: %{ct}ms ",
								  {
								    pf:agify(now, j.items[n].s.c),
								    ct:Math.round(j.items[n].s.u / 1000)
								  }
								);	
				}
			}
			s += "</tr></table>";

			st.innerHTML = s;
		}
		
		aging();
	
/*
	 try {
			ws.onopen = function() {
				console.log("ot_open.onopen");
			}
			ws.onmessage =function got_packet(msg) {
				// document.getElementById("result").textContent = msg.data;
				j = JSON.parse(msg.data);
				parse_json(j);
				
				display(j);
			}
			ws.onclose = function(e){
				console.log(" websocket connection CLOSED, code: " + e.code +
					    ", reason: " + e.reason);
			}
		} catch(exception) {
			alert('<p>Error' + exception);  
		}
		*/
}, false);

window.addEventListener("hashchange", function() {
	//console.log("hashchange");
	line_range_highlight(1);
}, false);

}());
