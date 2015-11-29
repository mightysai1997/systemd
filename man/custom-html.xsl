<?xml version='1.0'?> <!--*-nxml-*-->

<!--
  This file is part of systemd.

  Copyright 2011 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
-->

<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" version="1.0">
<xsl:import href="cross-refs-html.xsl"/>
<xsl:import href="permalink-id-scheme-html.xsl"/>
<xsl:import href="http://docbook.sourceforge.net/release/xsl/current/html/docbook.xsl"/>
<!--
  - The docbook stylesheet injects empty anchor tags into generated HTML, identified by an auto-generated ID.
  - Ask the docbook stylesheet to generate reproducible output when generating (these) ID values.
  - This makes the output of this stylesheet reproducible across identical invocations on the same input,
  - which is an easy and significant win for achieving reproducible builds.
  -
  - It may be even better to strip the empty anchors from the document output in addition to turning on consistent IDs,
  - for this stylesheet contains its own custom ID logic (for generating permalinks) already.
 -->
<xsl:param name="generate.consistent.ids" select="1"/>

<!--
  - Helper template to generate cross reference hyperlink
 -->
<xsl:template name="reflink">
  <xsl:param name="href"/>
  <a href="{$href}"><xsl:call-template name="inline.charseq"/></a>
</xsl:template>

<!-- translate man page references to links to html pages -->
<xsl:param name="cross.refs.debug" select="'0'"/>
<xsl:template match="citerefentry[not(@project)]">
  <xsl:variable name="crossID">
    <xsl:call-template name="determineCrossID"/>
  </xsl:variable>
  <xsl:call-template name="reflink">
    <xsl:with-param name="href">
      <xsl:value-of select="concat(refentrytitle,'.html')"/>
      <xsl:if test="string($crossID)!=''">
        <xsl:value-of select="concat('#',$crossID)"/>
      </xsl:if>
    </xsl:with-param>
  </xsl:call-template>

  <xsl:if test="$cross.refs.debug!='' and number($cross.refs.debug)!= 0">
    <blockquote><pre>Debug info:<br/>
        <xsl:call-template name="determineCrossID">
            <xsl:with-param name="cross-refs-debug" select="'debug'"/>
        </xsl:call-template>
    </pre></blockquote>
  </xsl:if>
</xsl:template>

<xsl:template match="citerefentry[@project='man-pages'] | citerefentry[manvolnum='2'] | citerefentry[manvolnum='4']">
  <xsl:call-template name="reflink">
    <xsl:with-param name="href" select="concat('http://man7.org/linux/man-pages/man', manvolnum, '/', refentrytitle, '.', manvolnum, '.html')"/>
  </xsl:call-template>
</xsl:template>

<xsl:template match="citerefentry[@project='die-net']">
  <xsl:call-template name="reflink">
    <xsl:with-param name="href" select="concat('http://linux.die.net/man/', manvolnum, '/', refentrytitle)"/>
  </xsl:call-template>
</xsl:template>

<xsl:template match="citerefentry[@project='mankier']">
  <xsl:call-template name="reflink">
    <xsl:with-param name="href" select="concat('https://www.mankier.com/', manvolnum, '/', refentrytitle)"/>
  </xsl:call-template>
</xsl:template>

<xsl:template match="citerefentry[@project='archlinux']">
  <xsl:call-template name="reflink">
    <xsl:with-param name="href" select="concat('https://www.archlinux.org/', refentrytitle, '/', refentrytitle, '.', manvolnum, '.html')"/>
  </xsl:call-template>
</xsl:template>

<xsl:template match="citerefentry[@project='freebsd']">
  <xsl:call-template name="reflink">
    <xsl:with-param name="href" select="concat('https://www.freebsd.org/cgi/man.cgi?', refentrytitle, '(', manvolnum, ')')"/>
  </xsl:call-template>
</xsl:template>

<xsl:template match="citerefentry[@project='dbus']">
  <xsl:call-template name="reflink">
    <xsl:with-param name="href" select="concat('http://dbus.freedesktop.org/doc/', refentrytitle, '.', manvolnum, '.html')"/>
  </xsl:call-template>
</xsl:template>

<xsl:template match="refsect1/title|refsect1/info/title">
  <!-- the ID is output in the block.object call for refsect1 -->
  <xsl:call-template name="headerlink">
    <xsl:with-param name="nodeType" select="'h2'"/>
  </xsl:call-template>
</xsl:template>

<xsl:template match="refsect2/title|refsect2/info/title">
  <xsl:call-template name="headerlink">
    <xsl:with-param name="nodeType" select="'h3'"/>
  </xsl:call-template>
</xsl:template>

<xsl:template match="varlistentry">
  <xsl:call-template name="permalink">
    <xsl:with-param name="nodeType" select="'dt'"/>
    <xsl:with-param name="linkTitle" select="'Permalink to this term'"/>
    <xsl:with-param name="nodeContent" select="term"/>
    <xsl:with-param name="keyNode" select="term[1]"/>
    <!--
      - To retain compatibility with IDs generated by previous versions of the template, inline.charseq must be called.
      - The purpose of that template is to generate markup (according to docbook documentation its purpose is to mark/format something as plain text).
      - The only reason to call this template is to get the auto-generated text such as brackets ([]) before flattening it.
     -->
    <xsl:with-param name="templateID">
      <xsl:call-template name="inline.charseq">
	<xsl:with-param name="content" select="term[1]"/>
      </xsl:call-template>
    </xsl:with-param>
  </xsl:call-template>
  <dd>
    <xsl:apply-templates select="listitem"/>
  </dd>
</xsl:template>


<!-- add Index link at top of page -->
<xsl:template name="user.header.content">
  <style>
    a.headerlink {
      color: #c60f0f;
      font-size: 0.8em;
      padding: 0 4px 0 4px;
      text-decoration: none;
      visibility: hidden;
    }

    a.headerlink:hover {
      background-color: #c60f0f;
      color: white;
    }

    h1:hover > a.headerlink, h2:hover > a.headerlink, h3:hover > a.headerlink, dt:hover > a.headerlink {
      visibility: visible;
    }
  </style>

  <a>
    <xsl:attribute name="href">
      <xsl:text>index.html</xsl:text>
    </xsl:attribute>
    <xsl:text>Index </xsl:text>
  </a>·
  <a>
    <xsl:attribute name="href">
      <xsl:text>systemd.directives.html</xsl:text>
    </xsl:attribute>
    <xsl:text>Directives </xsl:text>
  </a>

  <span style="float:right">
    <xsl:text>systemd </xsl:text>
    <xsl:value-of select="$systemd.version"/>
  </span>
  <hr/>
</xsl:template>

<xsl:template match="literal">
  <xsl:text>"</xsl:text>
  <xsl:call-template name="inline.monoseq"/>
  <xsl:text>"</xsl:text>
</xsl:template>

<!-- Switch things to UTF-8, ISO-8859-1 is soo yesteryear -->
<xsl:output method="html" encoding="UTF-8" indent="no"/>

</xsl:stylesheet>
