// Reserved words in field and label position, which is what ABI 15's reserved
// word sets are for.
package main

import (
	"errors"
	"fmt"
)

type Shape interface {
	Area() float64
}

type Rect struct {
	Width, Height float64
	Label         string
}

func (r Rect) Area() float64 { return r.Width * r.Height }

const (
	A = iota
	B
	C
)

var registry = map[string][]int{"a": {1, 2}, "b": nil}

func describe(s Shape) (string, error) {
	switch v := s.(type) {
	case Rect:
		return fmt.Sprintf("rect %v", v.Area()), nil
	default:
		return "", errors.New("unknown")
	}
}

func generic[T comparable](a, b T) bool { return a == b }

func run() {
	defer func() {
		if r := recover(); r != nil {
			fmt.Println(r)
		}
	}()
	ch := make(chan int, 1)
	go func() { ch <- 1 }()
	select {
	case v := <-ch:
		_ = v
	default:
	}
	for i := range 3 {
		_ = i
	}
}
