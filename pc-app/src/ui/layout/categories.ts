import { Hand, Palette, QrCode, ScanFace, type LucideIcon } from 'lucide-react'
import type { FunctionId } from './types'

/** Each rail item has a category colour (--cat-* in theme.css), as categories do in a block editor. */
export type Category = 'color' | 'tag' | 'face' | 'gesture' | 'settings' | 'firmware'

export const FN_CATEGORY: Record<FunctionId, Category> = { 1: 'color', 2: 'tag', 3: 'face', 8: 'gesture' }
export const FN_ICON: Record<FunctionId, LucideIcon> = { 1: Palette, 2: QrCode, 3: ScanFace, 8: Hand }
