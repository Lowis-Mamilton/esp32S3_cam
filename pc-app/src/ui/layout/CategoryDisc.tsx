import type { LucideIcon } from 'lucide-react'
import type { Category } from './categories'

/** Coloured disc with a white icon; decorative, the text label sits next to it. */
export default function CategoryDisc({ cat, Icon, size = 17 }: { cat: Category; Icon: LucideIcon; size?: number }) {
  return (
    <span className="nav-disc" style={{ background: `var(--cat-${cat})` }} aria-hidden="true">
      <Icon size={size} strokeWidth={2.25} />
    </span>
  )
}
