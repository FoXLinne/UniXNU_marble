import { h } from 'vue'
import DefaultTheme from 'vitepress/theme'
import MigrationNotice from './MigrationNotice.vue'
import './custom.css'

export default {
  extends: DefaultTheme,
  Layout: () => h(DefaultTheme.Layout, null, {
    'home-hero-before': () => h(MigrationNotice)
  })
}
