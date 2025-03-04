<!-- manpage-base.xsl:
     special formatting for manpages rendered from asciidoc+docbook -->
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                xmlns:exsl="http://exslt.org/common"
		version="1.0">

<!-- these params silence some output from xmlto -->
<xsl:param name="man.output.quietly" select="1"/>
<xsl:param name="refentry.meta.get.quietly" select="1"/>

<!-- template used to produce bold text with hyphenation disabled -->
<xsl:template name="bold-nh">
  <xsl:param name="node"/>
  <xsl:param name="context"/>
  <xsl:choose>
    <xsl:when test="not($context[ancestor::title])">
      <xsl:for-each select="$node/node()">
        <xsl:text>\fB\%</xsl:text>
        <xsl:apply-templates select="."/>
        <xsl:text>\fR</xsl:text>
      </xsl:for-each>
    </xsl:when>
    <xsl:otherwise>
      <xsl:apply-templates select="$node/node()"/>
    </xsl:otherwise>
  </xsl:choose>
</xsl:template>

<!-- template to produce references with hyphenation disabled -->
<xsl:template name="do-citerefentry-nh">
  <xsl:param name="refentrytitle" select="''"/>
  <xsl:param name="manvolnum" select="''"/>
  <xsl:variable name="title">
    <xsl:value-of select="$refentrytitle"/>
  </xsl:variable>
  <xsl:call-template name="bold-nh">
    <xsl:with-param name="node" select="exsl:node-set($title)"/>
    <xsl:with-param name="context" select="."/>
  </xsl:call-template>
  <xsl:text>(</xsl:text>
  <xsl:value-of select="$manvolnum"/>
  <xsl:text>)</xsl:text>
</xsl:template>

<!-- customized citerefentry template to produce references with
     hyphenation disabled -->
<xsl:template match="citerefentry">
  <xsl:call-template name="do-citerefentry-nh">
    <xsl:with-param name="refentrytitle" select="refentrytitle"/>
    <xsl:with-param name="manvolnum" select="manvolnum"/>
  </xsl:call-template>
</xsl:template>

<!-- convert asciidoc callouts to man page format;
     git.docbook.backslash and git.docbook.dot params
     must be supplied by another XSL file or other means -->
<xsl:template match="co">
	<xsl:value-of select="concat(
			      $git.docbook.backslash,'fB(',
			      substring-after(@id,'-'),')',
			      $git.docbook.backslash,'fR')"/>
</xsl:template>
<xsl:template match="calloutlist">
	<xsl:value-of select="$git.docbook.dot"/>
	<xsl:text>sp&#10;</xsl:text>
	<xsl:apply-templates/>
	<xsl:text>&#10;</xsl:text>
</xsl:template>
<xsl:template match="callout">
	<xsl:value-of select="concat(
			      $git.docbook.backslash,'fB',
			      substring-after(@arearefs,'-'),
			      '. ',$git.docbook.backslash,'fR')"/>
	<xsl:apply-templates/>
	<xsl:value-of select="$git.docbook.dot"/>
	<xsl:text>br&#10;</xsl:text>
</xsl:template>

</xsl:stylesheet>
