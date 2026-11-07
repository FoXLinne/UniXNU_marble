<script setup lang="ts">
import { useData, withBase } from 'vitepress'

const { frontmatter } = useData()
</script>

<template>
  <aside class="migration-notice" :aria-label="frontmatter.migration.title">
    <strong>{{ frontmatter.migration.title }}</strong>
    <p>
      {{ frontmatter.migration.text }}
      <a :href="withBase(frontmatter.migration.link)">{{ frontmatter.migration.label }}</a>
    </p>
  </aside>
</template>

<style scoped>
.migration-notice {
  margin: 24px auto 0;
  padding: 16px 20px;
  width: calc(100% - 48px);
  max-width: 1152px;
  border: 1px solid var(--vp-c-brand-1);
  border-left-width: 4px;
  border-radius: 6px;
  background: var(--vp-c-bg-alt);
  color: var(--vp-c-text-1);
  line-height: 1.6;
  animation: notice-enter 180ms ease-out;
}

p {
  margin: 4px 0 0;
}

a {
  color: var(--vp-c-brand-1);
  font-weight: 600;
  text-decoration: underline;
  text-underline-offset: 3px;
}

@keyframes notice-enter {
  from { opacity: 0; }
  to { opacity: 1; }
}

@media (prefers-reduced-motion: reduce) {
  .migration-notice { animation: none; }
}
</style>
