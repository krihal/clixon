#!/bin/bash
# Test: XML parser tests
#  @see https://www.w3.org/TR/2008/REC-xml-20081126
#       https://www.w3.org/TR/2009/REC-xml-names-20091208
#PROG="valgrind --leak-check=full --show-leak-kinds=all ../util/clixon_util_xml"


# include err() and new() functions and creates $dir
. ./lib.sh


PROG="../util/clixon_util_xml -D $DBG"

new "xml parse"
expecteof "$PROG" 0 "<a><b/></a>" "^<a><b/></a>$"

new "xml parse to json"
expecteof "$PROG -j" 0 "<a><b/></a>" '^{"a": {"b": null}}$'

new "xml parse strange names"
expecteof "$PROG" 0 "<_-><b0.><c-.-._/></b0.></_->" "^<_-><b0.><c-.-._/></b0.></_->$"

new "xml parse name errors"
expecteof "$PROG" 255 "<-a/>" ""

new "xml parse name errors"
expecteof "$PROG" 255 "<9/>" ""

new "xml parse name errors"
expecteof "$PROG" 255 "<a%/>" ""

LF='
'
new "xml parse content with CR LF -> LF, CR->LF (see https://www.w3.org/TR/REC-xml/#sec-line-ends)"
ret=$(echo "<x>ab${LF}c${LF}d</x>" | $PROG)
if [ "$ret" != "<x>a${LF}b${LF}c${LF}d</x>" ]; then
     err '<x>a$LFb$LFc</x>' "$ret"
fi

XML=$(cat <<EOF
<a><description>An example of escaped CENDs</description>
<sometext>
<![CDATA[ They're saying "x < y" & that "z > y" so I guess that means that z > x ]]>
</sometext>
<!-- This text contains a CEND ]]> -->
<!-- In this first case we put the ]] at the end of the first CDATA block
     and the > in the second CDATA block -->
<data><![CDATA[This text contains a CEND ]]]]><![CDATA[>]]></data>
<!-- In this second case we put a ] at the end of the first CDATA block
     and the ]> in the second CDATA block -->
<alternative><![CDATA[This text contains a CEND ]]]><![CDATA[]>]]></alternative>
</a>
EOF
)

new "xml CDATA"
expecteof "$PROG" 0 "$XML" "^<a><description>An example of escaped CENDs</description><sometext>
<![CDATA[ They're saying \"x < y\" & that \"z > y\" so I guess that means that z > x ]]>
</sometext><data><![CDATA[This text contains a CEND ]]]]><![CDATA[>]]></data><alternative><![CDATA[This text contains a CEND ]]]><![CDATA[]>]]></alternative></a>$"

XML=$(cat <<EOF
<message>Less than: &lt; , greater than: &gt; ampersand: &amp; </message>
EOF
)
new "xml encode <>&"
expecteof "$PROG" 0 "$XML" "^$XML$"

XML=$(cat <<EOF
<message>To allow attribute values to contain both single and double quotes, the apostrophe or single-quote character ' may be represented as &apos; and the double-quote character as &quot;</message>
EOF
)
new "xml optional encode single and double quote"
expecteof "$PROG" 0 "$XML" "^<message>To allow attribute values to contain both single and double quotes, the apostrophe or single-quote character ' may be represented as ' and the double-quote character as \"</message>$"

new "Double quotes for attributes"
expecteof "$PROG" 0 '<x a="t"/>' '^<x a="t"/>$'

new "Single quotes for attributes (returns double quotes but at least parses right)"
expecteof "$PROG" 0 "<x a='t'/>" '^<x a="t"/>$'

new "Mixed quotes"
expecteof "$PROG" 0 "<x a='t' b=\"q\"/>" '^<x a="t" b="q"/>$'

new "XMLdecl version"
expecteof "$PROG" 0 '<?xml version="1.0"?><a/>' '<a/>'

new "XMLdecl version, single quotes"
expecteof "$PROG" 0 "<?xml version='1.0'?><a/>" '<a/>'

new "XMLdecl version no element"
expecteof "$PROG" 255 '<?xml version="1.0"?>' ''

new "XMLdecl no version"
expecteof "$PROG" 255 '<?xml ?><a/>' ''

new "XMLdecl misspelled version"
expecteof "$PROG -l o" 255 '<?xml verion="1.0"?><a/>' 'yntax error: at or before: v'

new "XMLdecl version + encoding"
expecteof "$PROG" 0 '<?xml version="1.0" encoding="UTF-16"?><a/>' '<a/>'

new "XMLdecl version + misspelled encoding"
expecteof "$PROG -l o" 255 '<?xml version="1.0" encding="UTF-16"?><a/>' 'syntax error: at or before: e'

new "XMLdecl version + standalone"
expecteof "$PROG" 0 '<?xml version="1.0" standalone="yes"?><a/>' '<a/>'

new "PI - Processing instruction empty"
expecteof "$PROG" 0 '<?foo ?><a/>' '<a/>'

new "PI some content"
expecteof "$PROG" 0 '<?foo something else ?><a/>' '<a/>'

new "prolog element misc*"
expecteof "$PROG" 0 '<?foo something ?><a/><?bar more stuff ?><!-- a comment-->' '<a/>'

# We allow it as an internal necessity for parsing of xml fragments
#new "double element error"
#expecteof "$PROG" 255 '<a/><b/>' ''

new "namespace: DefaultAttName"
expecteof "$PROG" 0 '<x xmlns="n1">hello</x>' '^<x xmlns="n1">hello</x>$'

new "namespace: PrefixedAttName"
expecteof "$PROG" 0 '<x xmlns:n2="urn:example:des"><n2:y>hello</n2:y></x>' '^<x xmlns:n2="urn:example:des"><n2:y>hello</n2:y></x>$'

new "First example 6.1 from https://www.w3.org/TR/2009/REC-xml-names-20091208"
XML=$(cat <<EOF
<?xml version="1.0"?>

<html:html xmlns:html='http://www.w3.org/1999/xhtml'>

  <html:head><html:title>Frobnostication</html:title></html:head>
  <html:body><html:p>Moved to 
    <html:a href='http://frob.example.com'>here.</html:a></html:p></html:body>
</html:html>
EOF
)
expecteof "$PROG" 0 "$XML" "$XML"

new "Second example 6.1 from https://www.w3.org/TR/2009/REC-xml-names-20091208"
XML=$(cat <<EOF
<?xml version="1.0"?>
<!-- both namespace prefixes are available throughout -->
<bk:book xmlns:bk='urn:loc.gov:books'
         xmlns:isbn='urn:ISBN:0-395-36341-6'>
    <bk:title>Cheaper by the Dozen</bk:title>
    <isbn:number>1568491379</isbn:number>
</bk:book>
EOF
)
expecteof "$PROG" 0 "$XML" "$XML"
      
rm -rf $dir

