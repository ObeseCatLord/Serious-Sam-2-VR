package main

// A minimal reader/writer for Steam's text KeyValues (VDF) files, enough to set
// one app's launch options in userdata/<id>/config/localconfig.vdf.

import (
	"errors"
	"strings"
)

type vdfNode struct {
	key      string
	value    string     // A leaf's value.
	children []*vdfNode // A section's entries (nil for a leaf).
	section  bool
}

func vdfParse(text string) (*vdfNode, error) {
	p := &vdfParser{s: text}
	root := &vdfNode{section: true}
	if err := p.entries(root, true); err != nil {
		return nil, err
	}
	return root, nil
}

type vdfParser struct {
	s string
	i int
}

func (p *vdfParser) skip() {
	for p.i < len(p.s) {
		c := p.s[p.i]
		if c == ' ' || c == '\t' || c == '\r' || c == '\n' {
			p.i++
		} else if strings.HasPrefix(p.s[p.i:], "//") {
			for p.i < len(p.s) && p.s[p.i] != '\n' {
				p.i++
			}
		} else if strings.HasPrefix(p.s[p.i:], "\xef\xbb\xbf") {
			p.i += 3
		} else {
			return
		}
	}
}

func (p *vdfParser) str() (string, error) {
	p.skip()
	if p.i >= len(p.s) || p.s[p.i] != '"' {
		return "", errors.New("vdf: expected a quoted string")
	}
	p.i++
	var b strings.Builder
	for p.i < len(p.s) {
		c := p.s[p.i]
		if c == '\\' && p.i+1 < len(p.s) {
			b.WriteByte(c)
			b.WriteByte(p.s[p.i+1])
			p.i += 2
			continue
		}
		if c == '"' {
			p.i++
			return b.String(), nil // Escapes are kept as written (round trip).
		}
		b.WriteByte(c)
		p.i++
	}
	return "", errors.New("vdf: unterminated string")
}

func (p *vdfParser) entries(into *vdfNode, top bool) error {
	for {
		p.skip()
		if p.i >= len(p.s) {
			if top {
				return nil
			}
			return errors.New("vdf: unexpected end")
		}
		if p.s[p.i] == '}' {
			if top {
				return errors.New("vdf: unbalanced }")
			}
			p.i++
			return nil
		}
		key, err := p.str()
		if err != nil {
			return err
		}
		p.skip()
		if p.i < len(p.s) && p.s[p.i] == '{' {
			p.i++
			n := &vdfNode{key: key, section: true}
			if err := p.entries(n, false); err != nil {
				return err
			}
			into.children = append(into.children, n)
			continue
		}
		value, err := p.str()
		if err != nil {
			return err
		}
		into.children = append(into.children, &vdfNode{key: key, value: value})
	}
}

// child finds an entry by key (case-insensitive, as Steam does); create adds a section.
func (n *vdfNode) child(key string, create bool) *vdfNode {
	for _, c := range n.children {
		if strings.EqualFold(c.key, key) {
			return c
		}
	}
	if !create {
		return nil
	}
	c := &vdfNode{key: key, section: true}
	n.children = append(n.children, c)
	return c
}

func (n *vdfNode) path(create bool, keys ...string) *vdfNode {
	cur := n
	for _, k := range keys {
		if cur = cur.child(k, create); cur == nil || !cur.section {
			return nil
		}
	}
	return cur
}

func (n *vdfNode) leaf(key string) (*vdfNode, bool) {
	for _, c := range n.children {
		if strings.EqualFold(c.key, key) && !c.section {
			return c, true
		}
	}
	return nil, false
}

func (n *vdfNode) set(key, value string) {
	if c, ok := n.leaf(key); ok {
		c.value = value
		return
	}
	n.children = append(n.children, &vdfNode{key: key, value: value})
}

func vdfWrite(root *vdfNode) string {
	var b strings.Builder
	var write func(n *vdfNode, depth int)
	write = func(n *vdfNode, depth int) {
		for _, c := range n.children {
			indent := strings.Repeat("\t", depth)
			if c.section {
				b.WriteString(indent + "\"" + c.key + "\"\n" + indent + "{\n")
				write(c, depth+1)
				b.WriteString(indent + "}\n")
			} else {
				b.WriteString(indent + "\"" + c.key + "\"\t\t\"" + c.value + "\"\n")
			}
		}
	}
	write(root, 0)
	return b.String()
}

// vdfEscape escapes a value for writing (Steam escapes \ and ").
func vdfEscape(s string) string {
	return strings.NewReplacer(`\`, `\\`, `"`, `\"`).Replace(s)
}

func vdfUnescape(s string) string {
	return strings.NewReplacer(`\\`, `\`, `\"`, `"`).Replace(s)
}
