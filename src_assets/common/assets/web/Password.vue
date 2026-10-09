<template>
  <Navbar></Navbar>
  <div id="content" class="container">
    <div class="my-4">
      <h1>{{ $t('password.password_change') }}</h1>
      <p>{{ $t('password.password_change_desc') }}</p>
    </div>
    <form @submit.prevent="save">
      <div class="card">
        <div class="card-body">
          <div class="row">
            <div class="col-md-6">
              <h4>{{ $t('password.current_creds') }}</h4>
              <div class="mb-3">
                <label for="currentUsername" class="form-label">{{ $t('_common.username') }}</label>
                <input required type="text" class="form-control" id="currentUsername"
                  v-model="passwordData.currentUsername" />
              </div>
              <div class="mb-3">
                <label for="currentPassword" class="form-label">{{ $t('_common.password') }}</label>
                <input autocomplete="current-password" type="password" class="form-control" id="currentPassword"
                  v-model="passwordData.currentPassword" />
              </div>
            </div>
            <div class="col-md-6">
              <h4>{{ $t('password.new_creds') }}</h4>
              <div class="mb-3">
                <label for="newUsername" class="form-label">{{ $t('_common.username') }}</label>
                <input type="text" class="form-control" id="newUsername" v-model="passwordData.newUsername" />
                <div class="form-text">{{ $t('password.new_username_desc') }}</div>
              </div>
              <div class="mb-3">
                <label for="newPassword" class="form-label">{{ $t('_common.password') }}</label>
                <input autocomplete="new-password" required type="password" class="form-control" id="newPassword"
                  v-model="passwordData.newPassword" />
              </div>
              <div class="mb-3">
                <label for="confirmNewPassword" class="form-label">{{ $t('password.confirm_password') }}</label>
                <input autocomplete="new-password" required type="password" class="form-control" id="confirmNewPassword"
                  v-model="passwordData.confirmNewPassword" />
              </div>
            </div>
          </div>
        </div>
      </div>
      <div class="alert alert-danger my-3" v-if="error"><b>Error: </b>{{error}}</div>
      <div class="alert alert-success my-3" v-if="success">
        <b>{{ $t('_common.success') }}</b> {{ $t('password.success_msg') }}
      </div>
      <div class="mb-3 mt-4">
        <button type="submit" class="btn btn-primary" :disabled="loading">
          <save :size="18" class="icon"></save>
          {{ $t('_common.save') }}
        </button>
      </div>
    </form>
  </div>
</template>

<script>
  import Navbar from './Navbar.vue'
  import { saveCredentials } from './credentials'
  import { Save } from '@lucide/vue'

  export default {
    components: {
      Navbar,
      Save,
    },
    data() {
      return {
        error: null,
        success: false,
        loading: false,
        passwordData: {
          currentUsername: "",
          currentPassword: "",
          newUsername: "",
          newPassword: "",
          confirmNewPassword: "",
        },
      };
    },
    methods: {
      /**
       * @brief Save credentials and show validation or connection errors.
       * @return {Promise<void>} Resolves when the form submission has finished.
       */
      async save() {
        this.error = null;
        this.success = false;
        this.loading = true;
        try {
          await saveCredentials(this.passwordData);
          this.success = true;
          setTimeout(() => {
            document.location.reload();
          }, 5000);
        } catch (error) {
          this.error = error.message;
        } finally {
          this.loading = false;
        }
      },
    },
  }
</script>
