import { render, screen } from '@testing-library/react'
import { expect, test } from 'vitest'
import App from './App'

test('renders the MXCam Studio heading', () => {
  render(<App />)
  expect(screen.getByRole('heading', { name: 'MXCam Studio' })).toBeInTheDocument()
})

test('renders a single heading per test (cleanup between tests)', () => {
  render(<App />)
  expect(screen.getByRole('heading', { name: 'MXCam Studio' })).toBeInTheDocument()
})
